#pragma once

#include <windows.h>
#include <map>
#include <string>
#include <string_view>

class AudioEngine;

struct VolumeProfile {
    std::wstring hotkey;
    std::map<std::wstring, int> apps; // appId (lowercase) -> volume 0..100
};

struct AppConfig {
    int maxWidth = 360;
    int scrollStep = 2;
    bool showOnAllMonitors = true;
    std::wstring openPanelHotkey = L"Ctrl+Alt+V";
    std::map<std::wstring, VolumeProfile> profiles;
};

class ConfigManager {
public:
    ConfigManager() = default;

    std::wstring GetConfigPath() const;
    bool Load();
    bool Save() const;

    AppConfig& Data() { return cfg_; }
    const AppConfig& Data() const { return cfg_; }

    bool ApplyProfile(const std::wstring& profileName, AudioEngine& engine) const;
    bool SaveCurrentAsProfile(const std::wstring& profileName, const AudioEngine& engine);

    static bool ParseHotkey(std::wstring_view text, UINT* outMods, UINT* outVk);

    static bool IsAutostartEnabled();
    static bool SetAutostart(bool enable);

private:
    void SetDefaults();
    bool ParseJson(const std::string& utf8);
    std::string SerializeJson() const;

    AppConfig cfg_;
};
