#pragma once

#include <windows.h>

namespace msgs {

constexpr UINT WM_APP_AUDIO_DIRTY    = WM_APP + 10;
constexpr UINT WM_APP_VOLUME_CHANGED = WM_APP + 11;
constexpr UINT WM_APP_MASTER_CHANGED = WM_APP + 12;
constexpr UINT WM_APP_TOGGLE_PANEL   = WM_APP + 13;
constexpr UINT WM_APP_RELOAD_CONFIG  = WM_APP + 14;

} // namespace msgs
