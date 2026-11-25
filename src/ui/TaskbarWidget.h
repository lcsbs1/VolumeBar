#pragma once

#include <windows.h>
#include <memory>
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
    static constexpr wchar_t kBarWindowClass[] = L"VolumeBar_TaskbarBar";

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

    struct BarInstance {
        TaskbarWidget* owner = nullptr;
        HWND hwnd = nullptr;
        HWND tooltipHwnd = nullptr;
        HWND trayHwnd = nullptr;
        HMONITOR monitor = nullptr;
        bool isPrimary = false;
        UINT dpi = 96;
        bool barVisible = false;
        bool trackingMouse = false;
        int hoveredSlot = -1;
        RECT barRect{};
        std::vector<Slot> slots;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TooltipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild, DWORD thread, DWORD time);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleBarMessage(BarInstance& bar, UINT msg, WPARAM wParam, LPARAM lParam);

    void SyncTaskbars();
    std::unique_ptr<BarInstance> CreateBar(HWND trayHwnd, bool isPrimary);
    void DestroyBar(BarInstance& bar);

    bool QueryTaskbarRect(const BarInstance& bar, RECT* outRect, bool* outHidden) const;
    bool IsFullscreenAppActive(HMONITOR trayMon) const;
    void CheckVisibilityAndZOrder();
    void UpdateBarLayoutAndPosition(BarInstance& bar);

    RECT GetAnchorRectForBar(const BarInstance& bar) const;
    BarInstance* FindBarForMonitor(HMONITOR mon) const;

    void BuildSlots(BarInstance& bar);
    void RenderBar(BarInstance& bar);
    int HitTestSlot(const BarInstance& bar, POINT pt) const;

    void ShowTooltipForSlot(BarInstance& bar, int slotIdx, bool autoHideAfterScroll);
    void ShowTooltipText(BarInstance& bar, const std::wstring& text, POINT anchorScreenPt, bool autoHide);
    void HideTooltip(BarInstance& bar);
    void RenderTooltip(BarInstance& bar, const std::wstring& text, POINT anchorScreenPt);

    MenuCallbacks MakeMenuCallbacks();
    void OnShellEvent();

    AudioEngine& engine_;
    ConfigManager& config_;
    AppIconCache& icons_;
    MixerPanel& panel_;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    ui::Palette palette_;

    HWINEVENTHOOK hookForeground_ = nullptr;
    HWINEVENTHOOK hookReorder_ = nullptr;
    HWINEVENTHOOK hookMinimize_ = nullptr;

    std::vector<std::unique_ptr<BarInstance>> bars_;

    MasterAudioInfo master_;
    std::vector<AppAudioInfo> apps_;
    std::vector<std::wstring> hotkeyProfileNames_;
};
