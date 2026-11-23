#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "model/AppIcons.h"
#include "ui/Menus.h"
#include "ui/MixerPanel.h"
#include "ui/Theme.h"

class TaskbarWidget {
public:
    static constexpr wchar_t kWindowClass[] = L"VolumeBar_TaskbarWidget";

    TaskbarWidget(AudioEngine& engine, ConfigManager& config, AppIconCache& icons, MixerPanel& panel);
    ~TaskbarWidget();

    TaskbarWidget(const TaskbarWidget&) = delete;
    TaskbarWidget& operator=(const TaskbarWidget&) = delete;

    bool Create(HINSTANCE hInst);
    void Destroy();

    HWND GetHwnd() const { return hwnd_; }
    RECT GetAnchorRect() const;

    void RefreshData();
    void UpdateLayoutAndPosition();
    void UpdateTheme();
    void RegisterHotkeys();
    void UnregisterHotkeys();

private:
    enum class SlotKind { Master, App, Overflow };

    struct Slot {
        SlotKind kind = SlotKind::Master;
        int appIndex = -1;
        int overflowCount = 0;
        RECT rect{};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TooltipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild, DWORD thread, DWORD time);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool QueryTaskbarRect(RECT* outRect, bool* outHidden) const;
    bool IsFullscreenAppActive() const;
    void CheckVisibilityAndZOrder();

    void BuildSlots();
    void Render();
    int HitTestSlot(POINT pt) const;

    void ShowTooltipForSlot(int slotIdx, bool autoHideAfterScroll);
    void ShowTooltipText(const std::wstring& text, POINT anchorScreenPt, bool autoHide);
    void HideTooltip();
    void RenderTooltip(const std::wstring& text, POINT anchorScreenPt);

    MenuCallbacks MakeMenuCallbacks();
    void OnShellEvent();

    AudioEngine& engine_;
    ConfigManager& config_;
    AppIconCache& icons_;
    MixerPanel& panel_;

    HWND hwnd_ = nullptr;
    HWND tooltipHwnd_ = nullptr;
    UINT dpi_ = 96;
    UINT taskbarCreatedMsg_ = 0;
    ui::Palette palette_;

    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookReorder_ = nullptr;
    HWINEVENTHOOK hookMinimize_ = nullptr;

    bool barVisible_ = false;
    bool trackingMouse_ = false;
    int hoveredSlot_ = -1;
    RECT barRect_{};

    MasterAudioInfo master_;
    std::vector<AppAudioInfo> apps_;
    std::vector<Slot> slots_;
    std::vector<std::wstring> hotkeyProfileNames_;
};
