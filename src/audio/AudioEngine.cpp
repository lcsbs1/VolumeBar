#include <initguid.h>
#include "audio/AudioEngine.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <climits>

#include "util/Messages.h"
#include "util/Strings.h"

namespace {

// Custom context GUID so VolumeBar ignores echoes of its own SetMasterVolume / SetMute calls.
// {7F93A4C2-5D1E-4A89-9B0C-2D6F4E8A1107}
constexpr GUID kOurAudioContext = {
    0x7f93a4c2, 0x5d1e, 0x4a89, {0x9b, 0x0c, 0x2d, 0x6f, 0x4e, 0x8a, 0x11, 0x07}
};

constexpr size_t kMaxGroups = 64;

inline float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline bool EqualPropKey(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

std::wstring ReadDeviceFriendlyName(IMMDevice* device) {
    if (!device) return {};
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.ReleaseAndGetAddressOf()))) {
        return {};
    }
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) {
            name = pv.pwszVal;
        }
    }
    PropVariantClear(&pv);
    return name;
}

std::wstring CapitalizeStem(std::wstring_view stem) {
    std::wstring out(stem);
    if (!out.empty()) {
        CharUpperBuffW(out.data(), 1);
    }
    return out;
}

} // namespace

// --- COM Callback Sinks (never touch AudioEngine or Core Audio directly inside callbacks) ---

class AudioEngine::EndpointNotifClient final : public IMMNotificationClient {
public:
    explicit EndpointNotifClient(HWND hwnd) : hwnd_(hwnd) {}
    void Detach() { hwnd_.store(nullptr, std::memory_order_release); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        PostDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        PostDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        PostDirty();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && (role == eMultimedia || role == eConsole)) {
            PostDirty();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        if (EqualPropKey(key, PKEY_Device_FriendlyName)) {
            PostDirty();
        }
        return S_OK;
    }

private:
    void PostDirty() const {
        HWND h = hwnd_.load(std::memory_order_acquire);
        if (h) PostMessageW(h, msgs::WM_APP_AUDIO_DIRTY, 1, 0);
    }

    LONG refCount_ = 1;
    std::atomic<HWND> hwnd_{nullptr};
};

class AudioEngine::EndpointVolCallback final : public IAudioEndpointVolumeCallback {
public:
    explicit EndpointVolCallback(HWND hwnd) : hwnd_(hwnd) {}
    void Detach() { hwnd_.store(nullptr, std::memory_order_release); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IAudioEndpointVolumeCallback))) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    HRESULT STDMETHODCALLTYPE OnNotify(AUDIO_VOLUME_NOTIFICATION_DATA* pNotify) override {
        if (pNotify && IsEqualGUID(pNotify->guidEventContext, kOurAudioContext)) {
            return S_OK;
        }
        HWND h = hwnd_.load(std::memory_order_acquire);
        if (h) PostMessageW(h, msgs::WM_APP_MASTER_CHANGED, 0, 0);
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    std::atomic<HWND> hwnd_{nullptr};
};

class AudioEngine::SessionNotifSink final : public IAudioSessionNotification {
public:
    explicit SessionNotifSink(HWND hwnd) : hwnd_(hwnd) {}
    void Detach() { hwnd_.store(nullptr, std::memory_order_release); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IAudioSessionNotification))) {
            *ppv = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        HWND h = hwnd_.load(std::memory_order_acquire);
        if (h) PostMessageW(h, msgs::WM_APP_AUDIO_DIRTY, 0, 0);
        return S_OK;
    }

private:
    LONG refCount_ = 1;
    std::atomic<HWND> hwnd_{nullptr};
};

class AudioEngine::SessionEventsSink final : public IAudioSessionEvents {
public:
    explicit SessionEventsSink(HWND hwnd) : hwnd_(hwnd) {}
    void Detach() { hwnd_.store(nullptr, std::memory_order_release); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IAudioSessionEvents))) {
            *ppv = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return static_cast<ULONG>(c);
    }

    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID ctx) override {
        if (ctx && IsEqualGUID(*ctx, kOurAudioContext)) return S_OK;
        Post(msgs::WM_APP_AUDIO_DIRTY);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID ctx) override {
        if (ctx && IsEqualGUID(*ctx, kOurAudioContext)) return S_OK;
        Post(msgs::WM_APP_AUDIO_DIRTY);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, WINBOOL, LPCGUID ctx) override {
        if (ctx && IsEqualGUID(*ctx, kOurAudioContext)) return S_OK;
        Post(msgs::WM_APP_VOLUME_CHANGED);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override {
        Post(msgs::WM_APP_AUDIO_DIRTY);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
        Post(msgs::WM_APP_AUDIO_DIRTY);
        return S_OK;
    }

private:
    void Post(UINT msg) const {
        HWND h = hwnd_.load(std::memory_order_acquire);
        if (h) PostMessageW(h, msg, 0, 0);
    }

    LONG refCount_ = 1;
    std::atomic<HWND> hwnd_{nullptr};
};

// --- AudioEngine Implementation ---

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Initialize(HWND notifyHwnd) {
    Shutdown();
    shuttingDown_.store(false, std::memory_order_release);
    notifyHwnd_ = notifyHwnd;
    devicesDirty_.store(true, std::memory_order_release);

    policyConfig_.Initialize();

    std::lock_guard<std::mutex> lk(mtx_);
    HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        reinterpret_cast<void**>(enumerator_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !enumerator_) {
        return false;
    }

    if (notifyHwnd_) {
        endpointNotif_ = new EndpointNotifClient(notifyHwnd_);
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(endpointNotif_))) {
            endpointNotif_->Detach();
            endpointNotif_->Release();
            endpointNotif_ = nullptr;
        }
    }

    RebuildDevicesAndMasterLocked();
    RebuildSessionsLocked();
    return true;
}

void AudioEngine::Shutdown() {
    shuttingDown_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ClearSessionsLocked();
        ClearMasterLocked();
        ClearDevicesLocked();

        if (enumerator_ && endpointNotif_) {
            enumerator_->UnregisterEndpointNotificationCallback(endpointNotif_);
            endpointNotif_->Detach();
            endpointNotif_->Release();
            endpointNotif_ = nullptr;
        }
        enumerator_.Reset();
    }
    policyConfig_.Shutdown();
    notifyHwnd_ = nullptr;
}

void AudioEngine::MarkDevicesDirty() {
    devicesDirty_.store(true, std::memory_order_release);
}

void AudioEngine::PostNotify(UINT msg) const {
    if (notifyHwnd_) {
        PostMessageW(notifyHwnd_, msg, 0, 0);
    }
}

void AudioEngine::ClearSessionsLocked() {
    for (auto& g : groups_) {
        for (auto& s : g.sessions) {
            if (s.control && s.events) {
                s.control->UnregisterAudioSessionNotification(s.events);
            }
            if (s.events) {
                s.events->Detach();
                s.events->Release();
                s.events = nullptr;
            }
        }
    }
    groups_.clear();
}

void AudioEngine::ClearMasterLocked() {
    if (masterVolume_ && masterVolCallback_) {
        masterVolume_->UnregisterControlChangeNotify(masterVolCallback_);
    }
    if (masterVolCallback_) {
        masterVolCallback_->Detach();
        masterVolCallback_->Release();
        masterVolCallback_ = nullptr;
    }
    masterVolume_.Reset();
    masterMeter_.Reset();
    defaultDeviceId_.clear();
    defaultDeviceName_.clear();
}

void AudioEngine::ClearDevicesLocked() {
    for (auto& d : devices_) {
        if (d.sessionMgr && d.sessionNotif) {
            d.sessionMgr->UnregisterSessionNotification(d.sessionNotif);
        }
        if (d.sessionNotif) {
            d.sessionNotif->Detach();
            d.sessionNotif->Release();
            d.sessionNotif = nullptr;
        }
    }
    devices_.clear();
}

void AudioEngine::RebuildDevicesAndMasterLocked() {
    devicesDirty_.store(false, std::memory_order_release);
    ClearMasterLocked();
    ClearDevicesLocked();

    if (!enumerator_) return;

    ComPtr<IMMDevice> defDev;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, defDev.ReleaseAndGetAddressOf())) && defDev) {
        LPWSTR rawId = nullptr;
        if (SUCCEEDED(defDev->GetId(&rawId)) && rawId) {
            defaultDeviceId_ = rawId;
            CoTaskMemFree(rawId);
        }
        defaultDeviceName_ = ReadDeviceFriendlyName(defDev.Get());

        if (SUCCEEDED(defDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(masterVolume_.ReleaseAndGetAddressOf()))) &&
            masterVolume_) {
            float vol = 1.0f;
            BOOL muted = FALSE;
            if (SUCCEEDED(masterVolume_->GetMasterVolumeLevelScalar(&vol))) {
                cachedMasterVol_ = Clamp01(vol);
            }
            if (SUCCEEDED(masterVolume_->GetMute(&muted))) {
                cachedMasterMuted_ = (muted != FALSE);
            }
            if (notifyHwnd_) {
                masterVolCallback_ = new EndpointVolCallback(notifyHwnd_);
                if (FAILED(masterVolume_->RegisterControlChangeNotify(masterVolCallback_))) {
                    masterVolCallback_->Detach();
                    masterVolCallback_->Release();
                    masterVolCallback_ = nullptr;
                }
            }
        }
        defDev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(masterMeter_.ReleaseAndGetAddressOf()));
    }

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, coll.ReleaseAndGetAddressOf())) || !coll) {
        return;
    }

    UINT count = 0;
    if (FAILED(coll->GetCount(&count))) return;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.ReleaseAndGetAddressOf())) || !dev) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(dev->GetId(&rawId)) || !rawId) continue;
        std::wstring devId = rawId;
        CoTaskMemFree(rawId);

        DeviceEntry entry;
        entry.id = devId;
        entry.name = ReadDeviceFriendlyName(dev.Get());
        if (entry.name.empty()) entry.name = devId;
        entry.isDefault = (!defaultDeviceId_.empty() && devId == defaultDeviceId_);

        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(entry.sessionMgr.ReleaseAndGetAddressOf()))) &&
            entry.sessionMgr) {
            if (notifyHwnd_) {
                entry.sessionNotif = new SessionNotifSink(notifyHwnd_);
                if (FAILED(entry.sessionMgr->RegisterSessionNotification(entry.sessionNotif))) {
                    entry.sessionNotif->Detach();
                    entry.sessionNotif->Release();
                    entry.sessionNotif = nullptr;
                }
            }
            devices_.push_back(std::move(entry));
        }
    }
}

void AudioEngine::RebuildSessionsLocked() {
    ClearSessionsLocked();

    std::unordered_map<std::wstring, size_t> groupIdxById;

    for (const auto& dev : devices_) {
        if (!dev.sessionMgr) continue;

        ComPtr<IAudioSessionEnumerator> sessionEnum;
        if (FAILED(dev.sessionMgr->GetSessionEnumerator(sessionEnum.ReleaseAndGetAddressOf())) || !sessionEnum) {
            continue;
        }

        int count = 0;
        if (FAILED(sessionEnum->GetCount(&count)) || count <= 0) continue;

        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessionEnum->GetSession(i, ctrl.ReleaseAndGetAddressOf())) || !ctrl) continue;

            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(ctrl->GetState(&state))) continue;
            if (state == AudioSessionStateExpired) continue;

            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl.As(&ctrl2)) || !ctrl2) continue;

            bool isSystem = (ctrl2->IsSystemSoundsSession() == S_OK);
            DWORD pid = 0;
            ctrl2->GetProcessId(&pid);

            std::wstring exePath;
            if (!isSystem && pid != 0) {
                exePath = GetProcessExePath(pid);
            }

            LPWSTR rawDisp = nullptr;
            std::wstring sessionDisplay;
            if (SUCCEEDED(ctrl->GetDisplayName(&rawDisp)) && rawDisp) {
                sessionDisplay = ResolveIndirectString(rawDisp);
                CoTaskMemFree(rawDisp);
            }

            LPWSTR rawIcon = nullptr;
            std::wstring iconPath;
            if (SUCCEEDED(ctrl->GetIconPath(&rawIcon)) && rawIcon) {
                iconPath = rawIcon;
                CoTaskMemFree(rawIcon);
            }

            std::wstring appId;
            if (isSystem) {
                appId = L"system";
            } else if (!exePath.empty()) {
                std::wstring base = util::ToLower(util::BaseName(exePath));
                if (base == L"applicationframehost.exe" && !sessionDisplay.empty()) {
                    appId = L"uwp:" + util::ToLower(sessionDisplay);
                } else {
                    appId = base;
                }
            } else {
                LPWSTR rawSid = nullptr;
                std::wstring sid;
                if (SUCCEEDED(ctrl2->GetSessionIdentifier(&rawSid)) && rawSid) {
                    sid = rawSid;
                    CoTaskMemFree(rawSid);
                }
                if (!sessionDisplay.empty()) {
                    appId = L"app:" + util::ToLower(sessionDisplay);
                } else if (!sid.empty()) {
                    appId = L"sid:" + util::ToLower(sid);
                } else {
                    appId = L"pid:" + std::to_wstring(pid);
                }
            }

            ComPtr<ISimpleAudioVolume> simpleVol;
            if (FAILED(ctrl.As(&simpleVol)) || !simpleVol) continue;

            ComPtr<IAudioMeterInformation> meter;
            ctrl.As(&meter);

            SessionEntry se;
            se.control = ctrl2;
            se.volume = simpleVol;
            se.meter = meter;
            se.deviceId = dev.id;
            se.pid = pid;
            se.state = state;

            if (notifyHwnd_) {
                se.events = new SessionEventsSink(notifyHwnd_);
                if (FAILED(ctrl->RegisterAudioSessionNotification(se.events))) {
                    se.events->Detach();
                    se.events->Release();
                    se.events = nullptr;
                }
            }

            float vol = 1.0f;
            BOOL muted = FALSE;
            if (SUCCEEDED(simpleVol->GetMasterVolume(&vol))) vol = Clamp01(vol);
            simpleVol->GetMute(&muted);

            auto it = groupIdxById.find(appId);
            if (it == groupIdxById.end()) {
                if (groups_.size() >= kMaxGroups) {
                    if (se.events) {
                        ctrl->UnregisterAudioSessionNotification(se.events);
                        se.events->Detach();
                        se.events->Release();
                    }
                    continue;
                }

                AppGroupInternal g;
                g.info.appId = appId;
                g.info.displayName = MakeFriendlyName(sessionDisplay, exePath, isSystem);
                g.info.exePath = exePath;
                g.info.iconPath = iconPath;
                g.info.primaryDeviceId = dev.id;
                g.info.volume = vol;
                g.info.muted = (muted != FALSE);
                g.info.isActive = (state == AudioSessionStateActive);
                g.info.isSystemSounds = isSystem;
                if (pid != 0) g.info.pids.push_back(pid);

                if (isSystem) {
                    g.stableOrder = INT_MAX;
                } else {
                    auto ordIt = knownOrder_.find(appId);
                    if (ordIt == knownOrder_.end()) {
                        int assigned = nextOrder_++;
                        knownOrder_[appId] = assigned;
                        g.stableOrder = assigned;
                    } else {
                        g.stableOrder = ordIt->second;
                    }
                }

                g.sessions.push_back(std::move(se));
                groupIdxById[appId] = groups_.size();
                groups_.push_back(std::move(g));
            } else {
                AppGroupInternal& g = groups_[it->second];
                if (g.info.exePath.empty() && !exePath.empty()) g.info.exePath = exePath;
                if (g.info.iconPath.empty() && !iconPath.empty()) g.info.iconPath = iconPath;
                if (pid != 0 && std::find(g.info.pids.begin(), g.info.pids.end(), pid) == g.info.pids.end()) {
                    g.info.pids.push_back(pid);
                }
                if (state == AudioSessionStateActive) {
                    if (!g.info.isActive) {
                        g.info.volume = vol;
                        g.info.muted = (muted != FALSE);
                        g.info.primaryDeviceId = dev.id;
                    }
                    g.info.isActive = true;
                }
                g.sessions.push_back(std::move(se));
            }
        }
    }

    if (policyConfig_.IsAvailable()) {
        for (auto& g : groups_) {
            if (!g.info.isSystemSounds && !g.info.pids.empty()) {
                for (DWORD pid : g.info.pids) {
                    std::wstring persisted = policyConfig_.GetProcessOutputDevice(pid);
                    if (!persisted.empty()) {
                        g.info.persistedDeviceId = persisted;
                        break;
                    }
                }
            }
        }
    }

    std::sort(groups_.begin(), groups_.end(), [](const AppGroupInternal& a, const AppGroupInternal& b) {
        if (a.stableOrder != b.stableOrder) return a.stableOrder < b.stableOrder;
        return a.info.displayName < b.info.displayName;
    });
}

void AudioEngine::Refresh() {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (devicesDirty_.load(std::memory_order_acquire)) {
        RebuildDevicesAndMasterLocked();
    }
    RebuildSessionsLocked();
}

void AudioEngine::RefreshVolumes() {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& g : groups_) {
        const SessionEntry* chosen = nullptr;
        for (const auto& s : g.sessions) {
            if (s.state == AudioSessionStateActive) {
                chosen = &s;
                break;
            }
        }
        if (!chosen && !g.sessions.empty()) chosen = &g.sessions[0];
        if (chosen && chosen->volume) {
            float vol = g.info.volume;
            BOOL muted = g.info.muted ? TRUE : FALSE;
            if (SUCCEEDED(chosen->volume->GetMasterVolume(&vol))) {
                g.info.volume = Clamp01(vol);
            }
            if (SUCCEEDED(chosen->volume->GetMute(&muted))) {
                g.info.muted = (muted != FALSE);
            }
        }
    }
}

void AudioEngine::RefreshMaster() {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!masterVolume_) return;
    float vol = cachedMasterVol_;
    BOOL muted = cachedMasterMuted_ ? TRUE : FALSE;
    if (SUCCEEDED(masterVolume_->GetMasterVolumeLevelScalar(&vol))) {
        cachedMasterVol_ = Clamp01(vol);
    }
    if (SUCCEEDED(masterVolume_->GetMute(&muted))) {
        cachedMasterMuted_ = (muted != FALSE);
    }
}

MasterAudioInfo AudioEngine::GetMasterInfo() const {
    std::lock_guard<std::mutex> lk(mtx_);
    MasterAudioInfo info;
    info.volume = cachedMasterVol_;
    info.muted = cachedMasterMuted_;
    info.defaultDeviceId = defaultDeviceId_;
    info.defaultDeviceName = defaultDeviceName_;
    return info;
}

std::vector<AppAudioInfo> AudioEngine::GetApps() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<AppAudioInfo> out;
    out.reserve(groups_.size());
    for (const auto& g : groups_) {
        out.push_back(g.info);
    }
    return out;
}

std::vector<AudioDeviceInfo> AudioEngine::GetDevices() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<AudioDeviceInfo> out;
    out.reserve(devices_.size());
    for (const auto& d : devices_) {
        out.push_back({d.id, d.name, d.isDefault});
    }
    return out;
}

bool AudioEngine::SetMasterVolume(float level) {
    float clamped = Clamp01(level);
    std::lock_guard<std::mutex> lk(mtx_);
    if (!masterVolume_) return false;
    HRESULT hr = masterVolume_->SetMasterVolumeLevelScalar(clamped, &kOurAudioContext);
    if (SUCCEEDED(hr)) {
        cachedMasterVol_ = clamped;
        return true;
    }
    return false;
}

bool AudioEngine::SetMasterMute(bool muted) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!masterVolume_) return false;
    HRESULT hr = masterVolume_->SetMute(muted ? TRUE : FALSE, &kOurAudioContext);
    if (SUCCEEDED(hr)) {
        cachedMasterMuted_ = muted;
        return true;
    }
    return false;
}

bool AudioEngine::ToggleMasterMute() {
    bool target = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!masterVolume_) return false;
        target = !cachedMasterMuted_;
    }
    return SetMasterMute(target);
}

float AudioEngine::GetMasterPeak() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!masterMeter_) return 0.0f;
    float peak = 0.0f;
    if (SUCCEEDED(masterMeter_->GetPeakValue(&peak))) {
        return Clamp01(peak);
    }
    return 0.0f;
}

bool AudioEngine::SetAppVolume(const std::wstring& appId, float level) {
    float clamped = Clamp01(level);
    std::wstring key = util::ToLower(appId);
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& g : groups_) {
        if (g.info.appId == key) {
            bool anyOk = false;
            for (auto& s : g.sessions) {
                if (s.volume && SUCCEEDED(s.volume->SetMasterVolume(clamped, &kOurAudioContext))) {
                    anyOk = true;
                }
            }
            if (anyOk) g.info.volume = clamped;
            return anyOk;
        }
    }
    return false;
}

bool AudioEngine::SetAppMute(const std::wstring& appId, bool muted) {
    std::wstring key = util::ToLower(appId);
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& g : groups_) {
        if (g.info.appId == key) {
            bool anyOk = false;
            for (auto& s : g.sessions) {
                if (s.volume && SUCCEEDED(s.volume->SetMute(muted ? TRUE : FALSE, &kOurAudioContext))) {
                    anyOk = true;
                }
            }
            if (anyOk) g.info.muted = muted;
            return anyOk;
        }
    }
    return false;
}

bool AudioEngine::ToggleAppMute(const std::wstring& appId) {
    bool target = false;
    bool found = false;
    {
        std::wstring key = util::ToLower(appId);
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& g : groups_) {
            if (g.info.appId == key) {
                target = !g.info.muted;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;
    return SetAppMute(appId, target);
}

float AudioEngine::GetAppPeak(const std::wstring& appId) const {
    std::wstring key = util::ToLower(appId);
    std::lock_guard<std::mutex> lk(mtx_);
    float maxPeak = 0.0f;
    for (const auto& g : groups_) {
        if (g.info.appId == key) {
            for (const auto& s : g.sessions) {
                if (s.meter) {
                    float p = 0.0f;
                    if (SUCCEEDED(s.meter->GetPeakValue(&p))) {
                        maxPeak = (std::max)(maxPeak, Clamp01(p));
                    }
                }
            }
            break;
        }
    }
    return maxPeak;
}

bool AudioEngine::SetAppOutputDevice(const std::wstring& appId, const std::wstring& rawDeviceId) {
    if (!policyConfig_.IsAvailable()) return false;
    std::vector<DWORD> pids;
    std::wstring key = util::ToLower(appId);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& g : groups_) {
            if (g.info.appId == key) {
                if (g.info.isSystemSounds) return false;
                pids = g.info.pids;
                break;
            }
        }
    }
    if (pids.empty()) return false;
    bool ok = policyConfig_.SetAppOutputDevice(pids, rawDeviceId);
    if (ok) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& g : groups_) {
            if (g.info.appId == key) {
                g.info.persistedDeviceId = rawDeviceId;
                break;
            }
        }
    }
    return ok;
}

bool AudioEngine::FindAppId(const std::wstring& query, std::wstring* outAppId) const {
    if (!outAppId) return false;
    std::wstring q = util::ToLower(util::Trim(query));
    if (q.empty()) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& g : groups_) {
        if (g.info.appId == q) {
            *outAppId = g.info.appId;
            return true;
        }
    }
    for (const auto& g : groups_) {
        std::wstring stem = util::StripExtension(g.info.appId);
        if (stem == q || util::ToLower(g.info.displayName) == q) {
            *outAppId = g.info.appId;
            return true;
        }
    }
    for (const auto& g : groups_) {
        if (g.info.appId.find(q) != std::wstring::npos ||
            util::ToLower(g.info.displayName).find(q) != std::wstring::npos) {
            *outAppId = g.info.appId;
            return true;
        }
    }
    return false;
}

std::wstring AudioEngine::GetProcessExePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(wchar_t));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        result.assign(buf, len);
    }
    CloseHandle(proc);
    return result;
}

std::wstring AudioEngine::ResolveIndirectString(const std::wstring& source) {
    if (source.empty()) return {};
    if (source[0] != L'@') return source;
    wchar_t buf[512]{};
    if (SUCCEEDED(SHLoadIndirectString(source.c_str(), buf, static_cast<UINT>(sizeof(buf) / sizeof(wchar_t)), nullptr))) {
        return buf;
    }
    return {};
}

std::wstring AudioEngine::GetFileDescription(const std::wstring& exePath) {
    if (exePath.empty()) return {};
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (verSize == 0) return {};

    std::vector<BYTE> verData(verSize);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, verSize, verData.data())) {
        return {};
    }

    struct LangCodePage {
        WORD language;
        WORD codePage;
    };

    LangCodePage* trans = nullptr;
    UINT transLen = 0;
    if (VerQueryValueW(verData.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&trans), &transLen) &&
        trans && transLen >= sizeof(LangCodePage)) {
        wchar_t subBlock[64]{};
        wsprintfW(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                  trans[0].language, trans[0].codePage);
        wchar_t* desc = nullptr;
        UINT descLen = 0;
        if (VerQueryValueW(verData.data(), subBlock, reinterpret_cast<LPVOID*>(&desc), &descLen) &&
            desc && descLen > 0) {
            std::wstring cleaned = util::Trim(desc);
            if (!cleaned.empty()) return cleaned;
        }
    }

    // Fallback: common US English codepage
    wchar_t* desc = nullptr;
    UINT descLen = 0;
    if (VerQueryValueW(verData.data(), L"\\StringFileInfo\\040904b0\\FileDescription",
                       reinterpret_cast<LPVOID*>(&desc), &descLen) &&
        desc && descLen > 0) {
        return util::Trim(desc);
    }
    return {};
}

std::wstring AudioEngine::MakeFriendlyName(const std::wstring& sessionDisplayName,
                                           const std::wstring& exePath,
                                           bool isSystemSounds) {
    if (isSystemSounds) return L"Sons do sistema";

    std::wstring trimmedSession = util::Trim(sessionDisplayName);
    if (!trimmedSession.empty() && trimmedSession.find(L"%SystemRoot%") == std::wstring::npos) {
        return trimmedSession;
    }

    std::wstring desc = GetFileDescription(exePath);
    if (!desc.empty()) return desc;

    if (!exePath.empty()) {
        std::wstring stem = util::StripExtension(util::BaseName(exePath));
        if (!stem.empty()) return CapitalizeStem(stem);
    }
    return L"Aplicativo";
}
