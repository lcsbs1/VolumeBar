#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/AudioPolicyConfig.h"
#include "util/ComPtr.h"

#ifndef __IAudioMeterInformation_INTERFACE_DEFINED__
#define __IAudioMeterInformation_INTERFACE_DEFINED__
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IAudioMeterInformation, 0xc02216f6, 0x8c67, 0x4b5b, 0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64)
#endif
#endif

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AppAudioInfo {
    std::wstring appId;
    std::wstring displayName;
    std::wstring exePath;
    std::wstring iconPath;
    std::wstring primaryDeviceId;
    std::wstring persistedDeviceId; // non-empty if overridden via AudioPolicyConfig
    std::vector<DWORD> pids;
    float volume = 1.0f;
    bool muted = false;
    bool isActive = false;
    bool isSystemSounds = false;
};

struct MasterAudioInfo {
    float volume = 1.0f;
    bool muted = false;
    std::wstring defaultDeviceId;
    std::wstring defaultDeviceName;
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Must be called on a COM-initialized thread (STA on UI thread, or MTA/STA on CLI).
    bool Initialize(HWND notifyHwnd);
    void Shutdown();

    // Full or partial refreshes called on the owner thread when notified by COM callbacks.
    void MarkDevicesDirty();
    void Refresh();
    void RefreshVolumes();
    void RefreshMaster();

    MasterAudioInfo GetMasterInfo() const;
    std::vector<AppAudioInfo> GetApps() const;
    std::vector<AudioDeviceInfo> GetDevices() const;

    bool SetMasterVolume(float level);
    bool SetMasterMute(bool muted);
    bool ToggleMasterMute();
    float GetMasterPeak() const;

    bool SetAppVolume(const std::wstring& appId, float level);
    bool SetAppMute(const std::wstring& appId, bool muted);
    bool ToggleAppMute(const std::wstring& appId);
    float GetAppPeak(const std::wstring& appId) const;

    // Experimental per-app output routing
    bool IsPerAppRoutingAvailable() const { return policyConfig_.IsAvailable(); }
    const wchar_t* PerAppRoutingStatus() const { return policyConfig_.VariantName(); }
    bool SetAppOutputDevice(const std::wstring& appId, const std::wstring& rawDeviceId);

    // Match an app by exact appId, basename without .exe, or case-insensitive prefix/substring.
    bool FindAppId(const std::wstring& query, std::wstring* outAppId) const;

private:
    class EndpointNotifClient;
    class EndpointVolCallback;
    class SessionNotifSink;
    class SessionEventsSink;

    struct DeviceEntry {
        std::wstring id;
        std::wstring name;
        bool isDefault = false;
        ComPtr<IAudioSessionManager2> sessionMgr;
        SessionNotifSink* sessionNotif = nullptr; // owned via COM refcount
    };

    struct SessionEntry {
        ComPtr<IAudioSessionControl2> control;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioMeterInformation> meter;
        SessionEventsSink* events = nullptr; // owned via COM refcount
        std::wstring deviceId;
        DWORD pid = 0;
        AudioSessionState state = AudioSessionStateInactive;
    };

    struct AppGroupInternal {
        AppAudioInfo info;
        int stableOrder = 0;
        std::vector<SessionEntry> sessions;
    };

    void RebuildDevicesAndMasterLocked();
    void RebuildSessionsLocked();
    void ClearDevicesLocked();
    void ClearMasterLocked();
    void ClearSessionsLocked();
    void PostNotify(UINT msg) const;

    static std::wstring GetProcessExePath(DWORD pid);
    static std::wstring GetFileDescription(const std::wstring& exePath);
    static std::wstring ResolveIndirectString(const std::wstring& source);
    static std::wstring MakeFriendlyName(const std::wstring& sessionDisplayName,
                                         const std::wstring& exePath,
                                         bool isSystemSounds);

    HWND notifyHwnd_ = nullptr;
    std::atomic<bool> devicesDirty_{true};
    std::atomic<bool> shuttingDown_{false};

    mutable std::mutex mtx_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    EndpointNotifClient* endpointNotif_ = nullptr;

    std::wstring defaultDeviceId_;
    std::wstring defaultDeviceName_;
    ComPtr<IAudioEndpointVolume> masterVolume_;
    ComPtr<IAudioMeterInformation> masterMeter_;
    EndpointVolCallback* masterVolCallback_ = nullptr;
    float cachedMasterVol_ = 1.0f;
    bool cachedMasterMuted_ = false;

    std::vector<DeviceEntry> devices_;
    std::vector<AppGroupInternal> groups_;
    std::unordered_map<std::wstring, int> knownOrder_;
    int nextOrder_ = 0;

    AudioPolicyConfig policyConfig_;
};
