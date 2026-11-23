#pragma once

#include <windows.h>
#include <functional>
#include <string>

#include "audio/AudioEngine.h"
#include "config/Config.h"

struct MenuCallbacks {
    std::function<void()> onOpenPanel;
    std::function<void()> onReloadConfig;
    std::function<void()> onStateChanged;
    std::function<void()> onExit;
};

namespace ui {

void ShowGeneralMenu(HWND owner, POINT screenPt, AudioEngine& engine, ConfigManager& config,
                     const MenuCallbacks& cb);

void ShowAppMenu(HWND owner, POINT screenPt, const AppAudioInfo& app, AudioEngine& engine,
                 ConfigManager& config, const MenuCallbacks& cb);

void ShowDevicePickerMenu(HWND owner, POINT screenPt, const AppAudioInfo& app, AudioEngine& engine,
                          const std::function<void()>& onChanged);

bool PromptProfileName(HWND owner, std::wstring* outName);

} // namespace ui
