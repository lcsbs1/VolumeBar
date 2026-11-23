#include "audio/AudioPolicyConfig.h"

namespace {

using HStringHandle = void*;

// {ab3d4648-e242-459f-b02f-541c70306324} (Windows 10 21H2+ / Windows 11)
constexpr GUID kIidAudioPolicyConfig21H2 = {
    0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}
};

// {2a59116d-6c4f-45e0-a74f-707e3fef9258} (Windows 10 RS4 - 21H1)
constexpr GUID kIidAudioPolicyConfigDownlevel = {
    0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}
};

constexpr wchar_t kAudioPolicyClassName[] = L"Windows.Media.Internal.AudioPolicyConfig";
constexpr wchar_t kMmdevApiPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kRenderInterfaceSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
constexpr wchar_t kCaptureInterfaceSuffix[] = L"#{2eef81be-33fa-4800-9670-1cd474972c3f}";

// Mirrors EarTrumpet's IAudioPolicyConfigFactoryVariantFor21H2 / Downlevel vtable layout:
// 0..2  IUnknown
// 3..5  IInspectable (GetIids, GetRuntimeClassName, GetTrustLevel)
// 6..24 19 intermediate WinRT methods
// 25    SetPersistedDefaultAudioEndpoint
// 26    GetPersistedDefaultAudioEndpoint
// 27    ClearAllPersistedApplicationDefaultEndpoints
struct IAudioPolicyConfigFactoryVtbl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Slot03_GetIids() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot04_GetRuntimeClassName() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot05_GetTrustLevel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot06() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot07() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot08() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot09() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot10() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot11() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot12() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot13() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot14() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot15() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot16() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot17() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot18() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot19() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot20() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot21() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot22() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot23() = 0;
    virtual HRESULT STDMETHODCALLTYPE Slot24() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HStringHandle deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HStringHandle* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

} // namespace

AudioPolicyConfig::~AudioPolicyConfig() {
    Shutdown();
}

bool AudioPolicyConfig::Initialize() {
    Shutdown();

    combaseMod_ = LoadLibraryW(L"combase.dll");
    if (!combaseMod_) {
        variantName_ = L"unavailable (no combase)";
        return false;
    }

    roGetActivationFactory_ = reinterpret_cast<FnRoGetActivationFactory>(
        reinterpret_cast<void*>(GetProcAddress(combaseMod_, "RoGetActivationFactory")));
    windowsCreateString_ = reinterpret_cast<FnWindowsCreateString>(
        reinterpret_cast<void*>(GetProcAddress(combaseMod_, "WindowsCreateString")));
    windowsDeleteString_ = reinterpret_cast<FnWindowsDeleteString>(
        reinterpret_cast<void*>(GetProcAddress(combaseMod_, "WindowsDeleteString")));
    windowsGetStringRawBuffer_ = reinterpret_cast<FnWindowsGetStringRawBuffer>(
        reinterpret_cast<void*>(GetProcAddress(combaseMod_, "WindowsGetStringRawBuffer")));

    if (!roGetActivationFactory_ || !windowsCreateString_ || !windowsDeleteString_ || !windowsGetStringRawBuffer_) {
        Shutdown();
        variantName_ = L"unavailable (missing WinRT exports)";
        return false;
    }

    HStringHandle classId = nullptr;
    UINT32 len = static_cast<UINT32>(sizeof(kAudioPolicyClassName) / sizeof(wchar_t) - 1);
    if (FAILED(windowsCreateString_(kAudioPolicyClassName, len, &classId))) {
        Shutdown();
        variantName_ = L"unavailable (HSTRING)";
        return false;
    }

    void* rawFactory = nullptr;
    HRESULT hr = roGetActivationFactory_(classId, kIidAudioPolicyConfig21H2, &rawFactory);
    if (SUCCEEDED(hr) && rawFactory) {
        factory_ = static_cast<IUnknown*>(rawFactory);
        variantName_ = L"available (21H2+)";
        windowsDeleteString_(classId);
        return true;
    }

    hr = roGetActivationFactory_(classId, kIidAudioPolicyConfigDownlevel, &rawFactory);
    windowsDeleteString_(classId);
    if (SUCCEEDED(hr) && rawFactory) {
        factory_ = static_cast<IUnknown*>(rawFactory);
        variantName_ = L"available (downlevel)";
        return true;
    }

    Shutdown();
    variantName_ = L"unavailable (interface unsupported)";
    return false;
}

void AudioPolicyConfig::Shutdown() {
    if (factory_) {
        factory_->Release();
        factory_ = nullptr;
    }
    if (combaseMod_) {
        FreeLibrary(combaseMod_);
        combaseMod_ = nullptr;
    }
    roGetActivationFactory_ = nullptr;
    windowsCreateString_ = nullptr;
    windowsDeleteString_ = nullptr;
    windowsGetStringRawBuffer_ = nullptr;
    variantName_ = L"unavailable";
}

std::wstring AudioPolicyConfig::PackRenderDeviceId(const std::wstring& rawDeviceId) const {
    if (rawDeviceId.empty()) return {};
    std::wstring packed;
    packed.reserve(sizeof(kMmdevApiPrefix) / sizeof(wchar_t) + rawDeviceId.size() + sizeof(kRenderInterfaceSuffix) / sizeof(wchar_t));
    packed += kMmdevApiPrefix;
    packed += rawDeviceId;
    packed += kRenderInterfaceSuffix;
    return packed;
}

std::wstring AudioPolicyConfig::UnpackDeviceId(const std::wstring& winrtDeviceId) const {
    if (winrtDeviceId.empty()) return {};
    std::wstring out = winrtDeviceId;
    constexpr size_t prefixLen = (sizeof(kMmdevApiPrefix) / sizeof(wchar_t)) - 1;
    if (out.size() >= prefixLen && out.compare(0, prefixLen, kMmdevApiPrefix) == 0) {
        out.erase(0, prefixLen);
    }
    constexpr size_t renderSuffixLen = (sizeof(kRenderInterfaceSuffix) / sizeof(wchar_t)) - 1;
    if (out.size() >= renderSuffixLen &&
        out.compare(out.size() - renderSuffixLen, renderSuffixLen, kRenderInterfaceSuffix) == 0) {
        out.erase(out.size() - renderSuffixLen);
    }
    constexpr size_t captureSuffixLen = (sizeof(kCaptureInterfaceSuffix) / sizeof(wchar_t)) - 1;
    if (out.size() >= captureSuffixLen &&
        out.compare(out.size() - captureSuffixLen, captureSuffixLen, kCaptureInterfaceSuffix) == 0) {
        out.erase(out.size() - captureSuffixLen);
    }
    return out;
}

bool AudioPolicyConfig::SetProcessOutputDevice(DWORD pid, const std::wstring& rawDeviceId) const {
    if (!factory_ || pid == 0) return false;

    HStringHandle hDevice = nullptr;
    if (!rawDeviceId.empty()) {
        std::wstring packed = PackRenderDeviceId(rawDeviceId);
        if (FAILED(windowsCreateString_(packed.c_str(), static_cast<UINT32>(packed.size()), &hDevice))) {
            return false;
        }
    }

    auto* policy = static_cast<IAudioPolicyConfigFactoryVtbl*>(factory_);
    HRESULT hr1 = policy->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, hDevice);
    HRESULT hr2 = policy->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, hDevice);

    if (hDevice) {
        windowsDeleteString_(hDevice);
    }
    return SUCCEEDED(hr1) || SUCCEEDED(hr2);
}

bool AudioPolicyConfig::SetAppOutputDevice(const std::vector<DWORD>& pids, const std::wstring& rawDeviceId) const {
    if (!factory_ || pids.empty()) return false;
    bool anyOk = false;
    for (DWORD pid : pids) {
        if (SetProcessOutputDevice(pid, rawDeviceId)) {
            anyOk = true;
        }
    }
    return anyOk;
}

std::wstring AudioPolicyConfig::GetProcessOutputDevice(DWORD pid) const {
    if (!factory_ || pid == 0) return {};

    auto* policy = static_cast<IAudioPolicyConfigFactoryVtbl*>(factory_);
    HStringHandle hDevice = nullptr;
    HRESULT hr = policy->GetPersistedDefaultAudioEndpoint(
        pid, eRender, static_cast<ERole>(eMultimedia | eConsole), &hDevice);
    if (FAILED(hr) || !hDevice) {
        if (hDevice) windowsDeleteString_(hDevice);
        hr = policy->GetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, &hDevice);
    }
    if (FAILED(hr) || !hDevice) {
        if (hDevice) windowsDeleteString_(hDevice);
        return {};
    }

    UINT32 len = 0;
    const wchar_t* raw = windowsGetStringRawBuffer_(hDevice, &len);
    std::wstring result = (raw && len > 0) ? UnpackDeviceId(std::wstring(raw, len)) : std::wstring{};
    windowsDeleteString_(hDevice);
    return result;
}
