#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>

// Experimental per-app audio output endpoint routing using Windows' internal
// Windows.Media.Internal.AudioPolicyConfig WinRT factory.
// Fails gracefully if Microsoft changes the internal GUID/vtable in a future OS update.
class AudioPolicyConfig {
public:
    AudioPolicyConfig() = default;
    ~AudioPolicyConfig();

    AudioPolicyConfig(const AudioPolicyConfig&) = delete;
    AudioPolicyConfig& operator=(const AudioPolicyConfig&) = delete;

    bool Initialize();
    void Shutdown();

    bool IsAvailable() const { return factory_ != nullptr; }
    const wchar_t* VariantName() const { return variantName_; }

    // Pass empty rawDeviceId to reset the app back to the Windows default device.
    bool SetAppOutputDevice(const std::vector<DWORD>& pids, const std::wstring& rawDeviceId) const;
    bool SetProcessOutputDevice(DWORD pid, const std::wstring& rawDeviceId) const;

    // Returns raw IMMDevice id if overridden, or empty string if using Windows default / not found.
    std::wstring GetProcessOutputDevice(DWORD pid) const;

private:
    struct IUnknown* factory_ = nullptr;
    HMODULE combaseMod_ = nullptr;
    const wchar_t* variantName_ = L"unavailable";

    using FnRoGetActivationFactory = HRESULT(WINAPI*)(void* activatableClassId, REFIID iid, void** factory);
    using FnWindowsCreateString = HRESULT(WINAPI*)(const wchar_t* sourceString, UINT32 length, void** string);
    using FnWindowsDeleteString = HRESULT(WINAPI*)(void* string);
    using FnWindowsGetStringRawBuffer = const wchar_t*(WINAPI*)(void* string, UINT32* length);

    FnRoGetActivationFactory roGetActivationFactory_ = nullptr;
    FnWindowsCreateString windowsCreateString_ = nullptr;
    FnWindowsDeleteString windowsDeleteString_ = nullptr;
    FnWindowsGetStringRawBuffer windowsGetStringRawBuffer_ = nullptr;

    std::wstring PackRenderDeviceId(const std::wstring& rawDeviceId) const;
    std::wstring UnpackDeviceId(const std::wstring& winrtDeviceId) const;
};
