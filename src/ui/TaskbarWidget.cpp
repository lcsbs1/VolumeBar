#include "ui/TaskbarWidget.h"

#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <utility>

#include "resource.h"
#include "util/Messages.h"

namespace {

constexpr wchar_t kTooltipClass[] = L"VolumeBar_Tooltip";
constexpr UINT_PTR kDirtyDebounceTimerId = 10;
constexpr UINT_PTR kTooltipHideTimerId = 11;
constexpr int kHotkeyOpenPanel = 1;
constexpr int kHotkeyProfileBase = 100;

TaskbarWidget* gActiveWidget = nullptr;

inline int ToPct(float v) {
    int p = static_cast<int>(std::lround(v * 100.0f));
    return (std::max)(0, (std::min)(100, p));
}

bool IsIgnoredFullscreenClass(HWND hwnd) {
    wchar_t cls[128]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(wchar_t))) == 0) {
        return true;
    }
    return lstrcmpiW(cls, L"Progman") == 0 ||
           lstrcmpiW(cls, L"WorkerW") == 0 ||
           lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
           lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           lstrcmpiW(cls, L"TaskListThumbnailWnd") == 0 ||
           lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
           lstrcmpiW(cls, TaskbarWidget::kWindowClass) == 0 ||
           lstrcmpiW(cls, TaskbarWidget::kBarWindowClass) == 0;
}

struct EnumTrayTarget {
    HWND hwnd = nullptr;
    bool isPrimary = false;
};

BOOL CALLBACK EnumSecondaryTraysProc(HWND hwnd, LPARAM lParam) {
    auto* targets = reinterpret_cast<std::vector<EnumTrayTarget>*>(lParam);
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(wchar_t))) > 0) {
        if (lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 && IsWindowVisible(hwnd)) {
            targets->push_back({hwnd, false});
        }
    }
    return TRUE;
}

} // namespace

TaskbarWidget::TaskbarWidget(AudioEngine& engine, ConfigManager& config, AppIconCache& icons, MixerPanel& panel)
    : engine_(engine), config_(config), icons_(icons), panel_(panel) {
    palette_ = ui::MakePalette(ui::IsSystemDarkMode());
}

TaskbarWidget::~TaskbarWidget() {
    Destroy();
}

bool TaskbarWidget::Create(HINSTANCE hInst) {
    hInst_ = hInst;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW bwc{};
    bwc.cbSize = sizeof(bwc);
    bwc.lpfnWndProc = BarWndProc;
    bwc.hInstance = hInst;
    bwc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    bwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bwc.lpszClassName = kBarWindowClass;
    RegisterClassExW(&bwc);

    WNDCLASSEXW twc{};
    twc.cbSize = sizeof(twc);
    twc.lpfnWndProc = TooltipWndProc;
    twc.hInstance = hInst;
    twc.lpszClassName = kTooltipClass;
    RegisterClassExW(&twc);

    // Hidden coordinator window: permanent target for audio callbacks, hotkeys, and single-instance toggle.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClass, L"VolumeBar",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    panel_.SetCallbacks(MakeMenuCallbacks());

    gActiveWidget = this;
    hookForeground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                      nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hookReorder_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER,
                                   nullptr, WinEventProc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hookMinimize_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND,
                                    nullptr, WinEventProc, 0, 0,
                                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    RegisterHotkeys();
    SyncTaskbars();
    return true;
}

void TaskbarWidget::Destroy() {
    gActiveWidget = nullptr;
    if (hookForeground_) { UnhookWinEvent(hookForeground_); hookForeground_ = nullptr; }
    if (hookReorder_)    { UnhookWinEvent(hookReorder_);    hookReorder_ = nullptr; }
    if (hookMinimize_)   { UnhookWinEvent(hookMinimize_);   hookMinimize_ = nullptr; }

    UnregisterHotkeys();

    for (auto& bar : bars_) {
        if (bar) DestroyBar(*bar);
    }
    bars_.clear();

    if (hwnd_) {
        KillTimer(hwnd_, kDirtyDebounceTimerId);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

std::unique_ptr<TaskbarWidget::BarInstance> TaskbarWidget::CreateBar(HWND trayHwnd, bool isPrimary) {
    auto bar = std::make_unique<BarInstance>();
    bar->owner = this;
    bar->trayHwnd = trayHwnd;
    bar->isPrimary = isPrimary;
    bar->monitor = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);

    UINT trayDpi = GetDpiForWindow(trayHwnd);
    bar->dpi = (trayDpi != 0) ? trayDpi : 96;

    bar->hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kBarWindowClass, L"VolumeBar",
        WS_POPUP,
        0, 0, 100, 40,
        nullptr, nullptr, hInst_, bar.get());
    if (!bar->hwnd) return nullptr;

    bar->tooltipHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kTooltipClass, L"",
        WS_POPUP,
        0, 0, 10, 10,
        nullptr, nullptr, hInst_, nullptr);

    return bar;
}

void TaskbarWidget::DestroyBar(BarInstance& bar) {
    if (bar.tooltipHwnd) {
        DestroyWindow(bar.tooltipHwnd);
        bar.tooltipHwnd = nullptr;
    }
    if (bar.hwnd) {
        KillTimer(bar.hwnd, kTooltipHideTimerId);
        DestroyWindow(bar.hwnd);
        bar.hwnd = nullptr;
    }
    bar.barVisible = false;
}

void TaskbarWidget::SyncTaskbars() {
    if (!hwnd_) return;

    std::vector<EnumTrayTarget> targets;
    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        targets.push_back({primary, true});
    }
    if (config_.Data().showOnAllMonitors) {
        EnumWindows(EnumSecondaryTraysProc, reinterpret_cast<LPARAM>(&targets));
    }

    // Remove bars whose taskbar window is gone or no longer enabled.
    bars_.erase(
        std::remove_if(bars_.begin(), bars_.end(), [&](const std::unique_ptr<BarInstance>& bar) {
            if (!bar) return true;
            bool keep = false;
            for (const auto& t : targets) {
                if (t.hwnd == bar->trayHwnd && t.isPrimary == bar->isPrimary) {
                    keep = true;
                    break;
                }
            }
            if (!keep) {
                DestroyBar(*bar);
                return true;
            }
            return false;
        }),
        bars_.end());

    // Add bars for newly discovered taskbars.
    for (const auto& t : targets) {
        bool exists = false;
        for (const auto& bar : bars_) {
            if (bar && bar->trayHwnd == t.hwnd) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            if (auto created = CreateBar(t.hwnd, t.isPrimary)) {
                bars_.push_back(std::move(created));
            }
        }
    }
}

void TaskbarWidget::RegisterHotkeys() {
    if (!hwnd_) return;
    UnregisterHotkeys();

    UINT mods = 0;
    UINT vk = 0;
    if (ConfigManager::ParseHotkey(config_.Data().openPanelHotkey, &mods, &vk)) {
        RegisterHotKey(hwnd_, kHotkeyOpenPanel, mods, vk);
    }

    int idx = 0;
    for (const auto& kv : config_.Data().profiles) {
        if (!kv.second.hotkey.empty() && ConfigManager::ParseHotkey(kv.second.hotkey, &mods, &vk)) {
            if (RegisterHotKey(hwnd_, kHotkeyProfileBase + idx, mods, vk)) {
                hotkeyProfileNames_.push_back(kv.first);
                ++idx;
            }
        }
    }
}

void TaskbarWidget::UnregisterHotkeys() {
    if (!hwnd_) return;
    UnregisterHotKey(hwnd_, kHotkeyOpenPanel);
    for (size_t i = 0; i < hotkeyProfileNames_.size(); ++i) {
        UnregisterHotKey(hwnd_, kHotkeyProfileBase + static_cast<int>(i));
    }
    hotkeyProfileNames_.clear();
}

MenuCallbacks TaskbarWidget::MakeMenuCallbacks() {
    MenuCallbacks cb;
    cb.onOpenPanel = [this]() {
        panel_.Toggle(GetAnchorRect());
    };
    cb.onReloadConfig = [this]() {
        RegisterHotkeys();
        UpdateLayoutAndPosition();
        panel_.RefreshData();
    };
    cb.onStateChanged = [this]() {
        RefreshData();
        panel_.RefreshData();
    };
    cb.onExit = []() {
        PostQuitMessage(0);
    };
    return cb;
}

TaskbarWidget::BarInstance* TaskbarWidget::FindBarForMonitor(HMONITOR mon) const {
    for (const auto& bar : bars_) {
        if (bar && bar->monitor == mon && bar->barVisible) {
            return bar.get();
        }
    }
    for (const auto& bar : bars_) {
        if (bar && bar->monitor == mon) {
            return bar.get();
        }
    }
    return nullptr;
}

RECT TaskbarWidget::GetAnchorRectForBar(const BarInstance& bar) const {
    if (bar.barVisible && (bar.barRect.right > bar.barRect.left)) {
        return bar.barRect;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = bar.monitor ? bar.monitor : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(mon, &mi)) {
        return {mi.rcWork.left + 8, mi.rcWork.bottom, mi.rcWork.left + 200, mi.rcWork.bottom + 48};
    }
    return {8, 1032, 200, 1080};
}

RECT TaskbarWidget::GetAnchorRect() const {
    POINT cursor{};
    HMONITOR cursorMon = nullptr;
    if (GetCursorPos(&cursor)) {
        cursorMon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (BarInstance* bar = FindBarForMonitor(cursorMon)) {
            if (bar->barVisible && (bar->barRect.right > bar->barRect.left)) {
                return bar->barRect;
            }
        }
    }

    for (const auto& bar : bars_) {
        if (bar && bar->isPrimary && bar->barVisible && (bar->barRect.right > bar->barRect.left)) {
            return bar->barRect;
        }
    }
    for (const auto& bar : bars_) {
        if (bar && bar->barVisible && (bar->barRect.right > bar->barRect.left)) {
            return bar->barRect;
        }
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = cursorMon ? cursorMon : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(mon, &mi)) {
        return {mi.rcWork.left + 8, mi.rcWork.bottom, mi.rcWork.left + 200, mi.rcWork.bottom + 48};
    }
    return {8, 1032, 200, 1080};
}

bool TaskbarWidget::QueryTaskbarRect(const BarInstance& bar, RECT* outRect, bool* outHidden) const {
    if (!outRect || !outHidden) return false;
    *outHidden = false;

    if (!bar.trayHwnd || !IsWindow(bar.trayHwnd) || !IsWindowVisible(bar.trayHwnd)) {
        *outHidden = true;
        return false;
    }

    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT_PTR state = abd.hWnd ? SHAppBarMessage(ABM_GETSTATE, &abd) : 0;
    bool autoHide = (state & ABS_AUTOHIDE) != 0;

    RECT rc{};
    if (bar.isPrimary) {
        abd.hWnd = bar.trayHwnd;
        if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd) != 0) {
            rc = abd.rc;
        } else if (!GetWindowRect(bar.trayHwnd, &rc)) {
            *outHidden = true;
            return false;
        }
    } else {
        if (!GetWindowRect(bar.trayHwnd, &rc)) {
            *outHidden = true;
            return false;
        }
    }

    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        *outHidden = true;
        return false;
    }

    if (autoHide) {
        RECT trayRc{};
        if (GetWindowRect(bar.trayHwnd, &trayRc)) {
            HMONITOR mon = MonitorFromWindow(bar.trayHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                int visH = (std::min)(trayRc.bottom, mi.rcMonitor.bottom) - (std::max)(trayRc.top, mi.rcMonitor.top);
                int visW = (std::min)(trayRc.right, mi.rcMonitor.right) - (std::max)(trayRc.left, mi.rcMonitor.left);
                if (visH < 12 || visW < 12) {
                    *outHidden = true;
                    return false;
                }
            }
            rc = trayRc;
        }
    }

    *outRect = rc;
    return true;
}

bool TaskbarWidget::IsFullscreenAppActive(HMONITOR trayMon) const {
    if (!trayMon) return false;

    HWND fg = GetForegroundWindow();
    if (!fg || IsIgnoredFullscreenClass(fg)) return false;

    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (fgMon != trayMon) return false;

    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        if (quns == QUNS_RUNNING_D3D_FULL_SCREEN ||
            quns == QUNS_BUSY ||
            quns == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(trayMon, &mi)) return false;

    RECT fgRc{};
    if (!GetWindowRect(fg, &fgRc)) return false;

    return fgRc.left <= mi.rcMonitor.left &&
           fgRc.top <= mi.rcMonitor.top &&
           fgRc.right >= mi.rcMonitor.right &&
           fgRc.bottom >= mi.rcMonitor.bottom;
}

void TaskbarWidget::RefreshData() {
    master_ = engine_.GetMasterInfo();
    apps_ = engine_.GetApps();
    UpdateLayoutAndPosition();
}

void TaskbarWidget::UpdateTheme() {
    palette_ = ui::MakePalette(ui::IsSystemDarkMode());
    for (auto& bar : bars_) {
        if (bar && bar->barVisible) {
            RenderBar(*bar);
        }
    }
    panel_.UpdateTheme();
}

void TaskbarWidget::BuildSlots(BarInstance& bar) {
    bar.slots.clear();
    auto sc = [&bar](int dip) { return ui::ScaleDpi(dip, bar.dpi); };

    int barH = bar.barRect.bottom - bar.barRect.top;
    int slotW = sc(34);
    int padX = sc(4);
    int maxW = sc(config_.Data().maxWidth);
    if (maxW < slotW * 2) maxW = slotW * 2;

    int x = padX;

    // Slot 0: Master
    Slot mSlot;
    mSlot.kind = SlotKind::Master;
    mSlot.rect = {x, 0, x + slotW, barH};
    bar.slots.push_back(mSlot);
    x += slotW + sc(2);

    // Subtle divider space between Master and Apps
    x += sc(4);

    int totalApps = static_cast<int>(apps_.size());
    for (int i = 0; i < totalApps; ++i) {
        int remaining = totalApps - i;
        bool needOverflow = (remaining > 1) && (x + slotW * 2 + padX > maxW);
        if (needOverflow || (x + slotW + padX > maxW)) {
            Slot ov;
            ov.kind = SlotKind::Overflow;
            ov.overflowCount = remaining;
            ov.rect = {x, 0, x + sc(28), barH};
            bar.slots.push_back(ov);
            x += sc(28);
            break;
        }
        Slot s;
        s.kind = SlotKind::App;
        s.appIndex = i;
        s.rect = {x, 0, x + slotW, barH};
        bar.slots.push_back(s);
        x += slotW;
    }

    int finalW = (std::min)(maxW, x + padX);
    bar.barRect.right = bar.barRect.left + finalW;
}

void TaskbarWidget::UpdateBarLayoutAndPosition(BarInstance& bar) {
    if (!bar.hwnd) return;

    bar.monitor = MonitorFromWindow(bar.trayHwnd, MONITOR_DEFAULTTONEAREST);

    RECT trayRc{};
    bool hidden = false;
    if (!QueryTaskbarRect(bar, &trayRc, &hidden) || hidden || IsFullscreenAppActive(bar.monitor)) {
        if (bar.barVisible) {
            ShowWindow(bar.hwnd, SW_HIDE);
            bar.barVisible = false;
            HideTooltip(bar);
        }
        return;
    }

    UINT trayDpi = GetDpiForWindow(bar.trayHwnd);
    if (trayDpi != 0) {
        bar.dpi = trayDpi;
    }

    int leftMargin = ui::ScaleDpi(8, bar.dpi);
    bar.barRect.left = trayRc.left + leftMargin;
    bar.barRect.top = trayRc.top;
    bar.barRect.bottom = trayRc.bottom;
    bar.barRect.right = bar.barRect.left + ui::ScaleDpi(config_.Data().maxWidth, bar.dpi);

    BuildSlots(bar);

    int w = bar.barRect.right - bar.barRect.left;
    int h = bar.barRect.bottom - bar.barRect.top;

    SetWindowPos(bar.hwnd, HWND_TOPMOST, bar.barRect.left, bar.barRect.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    bar.barVisible = true;
    RenderBar(bar);
}

void TaskbarWidget::UpdateLayoutAndPosition() {
    if (!hwnd_) return;
    SyncTaskbars();
    for (auto& bar : bars_) {
        if (bar) UpdateBarLayoutAndPosition(*bar);
    }
}

void TaskbarWidget::CheckVisibilityAndZOrder() {
    if (!hwnd_) return;

    bool needSync = false;
    for (auto& bar : bars_) {
        if (!bar || !bar->hwnd) continue;
        if (!IsWindow(bar->trayHwnd)) {
            needSync = true;
            continue;
        }

        bar->monitor = MonitorFromWindow(bar->trayHwnd, MONITOR_DEFAULTTONEAREST);
        RECT trayRc{};
        bool hidden = false;
        if (!QueryTaskbarRect(*bar, &trayRc, &hidden) || hidden || IsFullscreenAppActive(bar->monitor)) {
            if (bar->barVisible) {
                ShowWindow(bar->hwnd, SW_HIDE);
                bar->barVisible = false;
                HideTooltip(*bar);
            }
            continue;
        }

        if (!bar->barVisible) {
            UpdateBarLayoutAndPosition(*bar);
        } else {
            SetWindowPos(bar->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        }
    }

    if (needSync) {
        UpdateLayoutAndPosition();
    }
}

void TaskbarWidget::RenderBar(BarInstance& bar) {
    if (!bar.hwnd || !bar.barVisible) return;

    int w = bar.barRect.right - bar.barRect.left;
    int h = bar.barRect.bottom - bar.barRect.top;
    if (w <= 0 || h <= 0) return;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, dib);
    auto sc = [&bar](int dip) { return ui::ScaleDpi(dip, bar.dpi); };

    {
        Gdiplus::Graphics g(memDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        // CRITICAL for UpdateLayeredWindow hit-testing: alpha=1 prevents clicks/scroll
        // from falling through to the taskbar between icons while remaining invisible.
        g.Clear(Gdiplus::Color(1, 0, 0, 0));

        int iconPx = sc(20);
        int barThick = (std::max)(2, sc(3));
        int iconTop = (h - (iconPx + sc(6) + barThick)) / 2;
        if (iconTop < sc(4)) iconTop = sc(4);
        int barTop = iconTop + iconPx + sc(4);

        for (size_t i = 0; i < bar.slots.size(); ++i) {
            const Slot& s = bar.slots[i];
            int sw = s.rect.right - s.rect.left;

            // Hover pill
            if (static_cast<int>(i) == bar.hoveredSlot) {
                Gdiplus::SolidBrush hoverBrush(palette_.hoverFill);
                Gdiplus::RectF hr(static_cast<float>(s.rect.left + sc(1)),
                                  static_cast<float>(sc(4)),
                                  static_cast<float>(sw - sc(2)),
                                  static_cast<float>(h - sc(8)));
                ui::FillRoundedRect(g, hoverBrush, hr, static_cast<float>(sc(6)));
            }

            if (s.kind == SlotKind::Master) {
                float ix = static_cast<float>(s.rect.left + (sw - iconPx) / 2);
                Gdiplus::RectF spkRc(ix, static_cast<float>(iconTop),
                                     static_cast<float>(iconPx), static_cast<float>(iconPx));
                ui::DrawSpeakerGlyph(g, spkRc, master_.volume, master_.muted,
                                     palette_.textPrimary, palette_.mutedRed);

                // Volume bar under Master
                float bx = static_cast<float>(s.rect.left + sc(5));
                float bw = static_cast<float>(sw - sc(10));
                Gdiplus::SolidBrush track(palette_.trackBg);
                ui::FillRoundedRect(g, track, Gdiplus::RectF(bx, static_cast<float>(barTop), bw, static_cast<float>(barThick)),
                                    static_cast<float>(barThick) * 0.5f);
                float fw = bw * (master_.muted ? 0.0f : master_.volume);
                if (fw > 1.0f || master_.muted) {
                    Gdiplus::SolidBrush fill(master_.muted ? palette_.mutedRed : palette_.accent);
                    ui::FillRoundedRect(g, fill,
                                        Gdiplus::RectF(bx, static_cast<float>(barTop), master_.muted ? bw : fw, static_cast<float>(barThick)),
                                        static_cast<float>(barThick) * 0.5f);
                }

                // Subtle vertical separator after Master
                Gdiplus::Pen sepPen(palette_.border, 1.0f);
                float sepX = static_cast<float>(s.rect.right + sc(2));
                g.DrawLine(&sepPen, sepX, static_cast<float>(sc(10)), sepX, static_cast<float>(h - sc(10)));
            } else if (s.kind == SlotKind::App && s.appIndex >= 0 && s.appIndex < static_cast<int>(apps_.size())) {
                const AppAudioInfo& app = apps_[static_cast<size_t>(s.appIndex)];
                Gdiplus::Bitmap* bmp = icons_.GetIcon(app, iconPx);
                float ix = static_cast<float>(s.rect.left + (sw - iconPx) / 2);

                if (bmp) {
                    if (app.isActive && !app.muted) {
                        g.DrawImage(bmp, Gdiplus::RectF(ix, static_cast<float>(iconTop),
                                                        static_cast<float>(iconPx), static_cast<float>(iconPx)));
                    } else if (app.muted) {
                        // Desaturated + dimmed for muted apps
                        Gdiplus::ColorMatrix cm = {{
                            {0.30f, 0.30f, 0.30f, 0, 0},
                            {0.59f, 0.59f, 0.59f, 0, 0},
                            {0.11f, 0.11f, 0.11f, 0, 0},
                            {0,     0,     0,     0.42f, 0},
                            {0,     0,     0,     0, 1}
                        }};
                        Gdiplus::ImageAttributes ia;
                        ia.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                        g.DrawImage(bmp, Gdiplus::RectF(ix, static_cast<float>(iconTop),
                                                        static_cast<float>(iconPx), static_cast<float>(iconPx)),
                                    0, 0, static_cast<Gdiplus::REAL>(bmp->GetWidth()),
                                    static_cast<Gdiplus::REAL>(bmp->GetHeight()),
                                    Gdiplus::UnitPixel, &ia);
                    } else {
                        // Discreet dimmed icon for paused/inactive apps
                        Gdiplus::ColorMatrix cm = {{
                            {1, 0, 0, 0, 0},
                            {0, 1, 0, 0, 0},
                            {0, 0, 1, 0, 0},
                            {0, 0, 0, 0.48f, 0},
                            {0, 0, 0, 0, 1}
                        }};
                        Gdiplus::ImageAttributes ia;
                        ia.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                        g.DrawImage(bmp, Gdiplus::RectF(ix, static_cast<float>(iconTop),
                                                        static_cast<float>(iconPx), static_cast<float>(iconPx)),
                                    0, 0, static_cast<Gdiplus::REAL>(bmp->GetWidth()),
                                    static_cast<Gdiplus::REAL>(bmp->GetHeight()),
                                    Gdiplus::UnitPixel, &ia);
                    }
                }

                // Thin 3px volume bar under app icon
                float bx = static_cast<float>(s.rect.left + sc(5));
                float bw = static_cast<float>(sw - sc(10));
                Gdiplus::SolidBrush track(app.isActive ? palette_.trackBg : Gdiplus::Color(48, 255, 255, 255));
                ui::FillRoundedRect(g, track, Gdiplus::RectF(bx, static_cast<float>(barTop), bw, static_cast<float>(barThick)),
                                    static_cast<float>(barThick) * 0.5f);

                if (app.muted) {
                    Gdiplus::SolidBrush redBrush(palette_.mutedRed);
                    ui::FillRoundedRect(g, redBrush, Gdiplus::RectF(bx, static_cast<float>(barTop), bw, static_cast<float>(barThick)),
                                        static_cast<float>(barThick) * 0.5f);
                } else {
                    float fw = bw * app.volume;
                    if (fw > 1.0f) {
                        Gdiplus::SolidBrush fill(app.isActive ? palette_.accent : palette_.accentDim);
                        ui::FillRoundedRect(g, fill, Gdiplus::RectF(bx, static_cast<float>(barTop), fw, static_cast<float>(barThick)),
                                            static_cast<float>(barThick) * 0.5f);
                    }
                }
            } else if (s.kind == SlotKind::Overflow) {
                Gdiplus::FontFamily ff(L"Segoe UI");
                Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(sc(11)), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush fg(palette_.textSecondary);
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                std::wstring txt = L"+" + std::to_wstring(s.overflowCount);
                Gdiplus::RectF tr(static_cast<float>(s.rect.left), 0.0f, static_cast<float>(sw), static_cast<float>(h));
                g.DrawString(txt.c_str(), -1, &font, tr, &sf, &fg);
            }
        }
    }

    POINT ptSrc = {0, 0};
    POINT ptDst = {bar.barRect.left, bar.barRect.top};
    SIZE sz = {w, h};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(bar.hwnd, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

int TaskbarWidget::HitTestSlot(const BarInstance& bar, POINT pt) const {
    for (size_t i = 0; i < bar.slots.size(); ++i) {
        const RECT& r = bar.slots[i].rect;
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TaskbarWidget::ShowTooltipForSlot(BarInstance& bar, int slotIdx, bool autoHideAfterScroll) {
    if (slotIdx < 0 || slotIdx >= static_cast<int>(bar.slots.size())) {
        HideTooltip(bar);
        return;
    }

    const Slot& s = bar.slots[static_cast<size_t>(slotIdx)];
    std::wstring text;
    if (s.kind == SlotKind::Master) {
        text = L"Mestre — " + std::to_wstring(ToPct(master_.volume)) + L"%";
        if (master_.muted) text += L" (Mudo)";
    } else if (s.kind == SlotKind::App && s.appIndex >= 0 && s.appIndex < static_cast<int>(apps_.size())) {
        const AppAudioInfo& app = apps_[static_cast<size_t>(s.appIndex)];
        text = app.displayName + L" — " + std::to_wstring(ToPct(app.volume)) + L"%";
        if (app.muted) text += L" (Mudo)";
        else if (!app.isActive) text += L" (Pausado)";
    } else if (s.kind == SlotKind::Overflow) {
        text = L"Abrir mixer (+" + std::to_wstring(s.overflowCount) + L" apps)";
    }

    POINT anchor = {bar.barRect.left + (s.rect.left + s.rect.right) / 2, bar.barRect.top};
    ShowTooltipText(bar, text, anchor, autoHideAfterScroll);
}

void TaskbarWidget::ShowTooltipText(BarInstance& bar, const std::wstring& text, POINT anchorScreenPt, bool autoHide) {
    if (!bar.tooltipHwnd || text.empty()) return;
    RenderTooltip(bar, text, anchorScreenPt);
    if (autoHide && bar.hwnd) {
        SetTimer(bar.hwnd, kTooltipHideTimerId, 1300, nullptr);
    } else if (bar.hwnd) {
        KillTimer(bar.hwnd, kTooltipHideTimerId);
    }
}

void TaskbarWidget::HideTooltip(BarInstance& bar) {
    if (bar.hwnd) KillTimer(bar.hwnd, kTooltipHideTimerId);
    if (bar.tooltipHwnd) ShowWindow(bar.tooltipHwnd, SW_HIDE);
}

void TaskbarWidget::RenderTooltip(BarInstance& bar, const std::wstring& text, POINT anchorScreenPt) {
    auto sc = [&bar](int dip) { return ui::ScaleDpi(dip, bar.dpi); };

    HDC screenDc = GetDC(nullptr);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(sc(12)), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    Gdiplus::RectF bounds;
    {
        Gdiplus::Graphics measureG(screenDc);
        measureG.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &bounds);
    }

    int padX = sc(10);
    int padY = sc(6);
    int w = static_cast<int>(std::ceil(bounds.Width)) + padX * 2;
    int h = static_cast<int>(std::ceil(bounds.Height)) + padY * 2;
    if (w < sc(48)) w = sc(48);
    if (h < sc(26)) h = sc(26);

    HMONITOR mon = MonitorFromPoint(anchorScreenPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int x = anchorScreenPt.x - w / 2;
    int y = anchorScreenPt.y - h - sc(8);
    if (x < mi.rcMonitor.left + sc(4)) x = mi.rcMonitor.left + sc(4);
    if (x + w > mi.rcMonitor.right - sc(4)) x = mi.rcMonitor.right - w - sc(4);
    if (y < mi.rcMonitor.top + sc(4)) y = bar.barRect.bottom + sc(8);

    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HGDIOBJ oldBmp = SelectObject(memDc, dib);
        {
            Gdiplus::Graphics g(memDc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));

            Gdiplus::RectF r(0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);
            Gdiplus::SolidBrush bg(palette_.tooltipBg);
            Gdiplus::Pen border(palette_.tooltipBorder, 1.0f);
            ui::FillRoundedRect(g, bg, r, static_cast<float>(sc(6)));
            ui::DrawRoundedRect(g, border, r, static_cast<float>(sc(6)));

            Gdiplus::SolidBrush fg(palette_.textPrimary);
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            g.DrawString(text.c_str(), -1, &font, Gdiplus::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), &sf, &fg);
        }

        POINT ptSrc = {0, 0};
        POINT ptDst = {x, y};
        SIZE sz = {w, h};
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        SetWindowPos(bar.tooltipHwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateLayeredWindow(bar.tooltipHwnd, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
    }

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void TaskbarWidget::OnShellEvent() {
    CheckVisibilityAndZOrder();
}

void CALLBACK TaskbarWidget::WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (gActiveWidget) {
        gActiveWidget->OnShellEvent();
    }
}

LRESULT CALLBACK TaskbarWidget::TooltipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TaskbarWidget::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<TaskbarWidget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TaskbarWidget*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TaskbarWidget::BarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* bar = reinterpret_cast<BarInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        bar = static_cast<BarInstance*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bar));
        bar->hwnd = hwnd;
    }
    if (bar && bar->owner) return bar->owner->HandleBarMessage(*bar, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TaskbarWidget::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        UpdateLayoutAndPosition();
        return 0;
    }

    switch (msg) {
        case msgs::WM_APP_AUDIO_DIRTY:
            if (wParam == 1) {
                engine_.MarkDevicesDirty();
            }
            SetTimer(hwnd_, kDirtyDebounceTimerId, 50, nullptr);
            return 0;

        case msgs::WM_APP_VOLUME_CHANGED:
            engine_.RefreshVolumes();
            RefreshData();
            panel_.RefreshData();
            return 0;

        case msgs::WM_APP_MASTER_CHANGED:
            engine_.RefreshMaster();
            RefreshData();
            panel_.RefreshData();
            return 0;

        case msgs::WM_APP_TOGGLE_PANEL:
            panel_.Toggle(GetAnchorRect());
            return 0;

        case msgs::WM_APP_RELOAD_CONFIG:
            config_.Load();
            RegisterHotkeys();
            UpdateLayoutAndPosition();
            panel_.RefreshData();
            return 0;

        case WM_TIMER:
            if (wParam == kDirtyDebounceTimerId) {
                KillTimer(hwnd_, kDirtyDebounceTimerId);
                engine_.Refresh();
                RefreshData();
                panel_.RefreshData();
                return 0;
            }
            break;

        case WM_HOTKEY: {
            int id = static_cast<int>(wParam);
            if (id == kHotkeyOpenPanel) {
                panel_.Toggle(GetAnchorRect());
                return 0;
            }
            if (id >= kHotkeyProfileBase) {
                size_t idx = static_cast<size_t>(id - kHotkeyProfileBase);
                if (idx < hotkeyProfileNames_.size()) {
                    const std::wstring& profName = hotkeyProfileNames_[idx];
                    config_.ApplyProfile(profName, engine_);
                    RefreshData();
                    panel_.RefreshData();

                    BarInstance* targetBar = nullptr;
                    POINT cursor{};
                    if (GetCursorPos(&cursor)) {
                        targetBar = FindBarForMonitor(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
                    }
                    if (!targetBar && !bars_.empty()) {
                        targetBar = bars_.front().get();
                    }
                    if (targetBar && targetBar->barVisible) {
                        POINT anchor = {
                            targetBar->barRect.left + (targetBar->barRect.right - targetBar->barRect.left) / 2,
                            targetBar->barRect.top
                        };
                        ShowTooltipText(*targetBar, L"Perfil: " + profName, anchor, true);
                    }
                }
                return 0;
            }
            break;
        }

        case WM_DISPLAYCHANGE:
            UpdateLayoutAndPosition();
            return 0;

        case WM_SETTINGCHANGE:
            UpdateTheme();
            UpdateLayoutAndPosition();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

LRESULT TaskbarWidget::HandleBarMessage(BarInstance& bar, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kTooltipHideTimerId) {
                HideTooltip(bar);
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            if (!bar.trackingMouse) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = bar.hwnd;
                TrackMouseEvent(&tme);
                bar.trackingMouse = true;
            }
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(bar, pt);
            if (idx != bar.hoveredSlot) {
                bar.hoveredSlot = idx;
                RenderBar(bar);
                if (idx >= 0) ShowTooltipForSlot(bar, idx, false);
                else HideTooltip(bar);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            bar.trackingMouse = false;
            if (bar.hoveredSlot != -1) {
                bar.hoveredSlot = -1;
                RenderBar(bar);
            }
            HideTooltip(bar);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(bar.hwnd, &pt);
            int idx = HitTestSlot(bar, pt);
            if (idx < 0) return 0;

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
            float step = static_cast<float>(shift ? 10 : config_.Data().scrollStep) / 100.0f;
            float dir = (delta > 0) ? 1.0f : -1.0f;

            const Slot& s = bar.slots[static_cast<size_t>(idx)];
            if (s.kind == SlotKind::Master) {
                engine_.SetMasterVolume(master_.volume + dir * step);
            } else if (s.kind == SlotKind::App && s.appIndex >= 0 && s.appIndex < static_cast<int>(apps_.size())) {
                const AppAudioInfo& app = apps_[static_cast<size_t>(s.appIndex)];
                engine_.SetAppVolume(app.appId, app.volume + dir * step);
            } else {
                return 0;
            }

            master_ = engine_.GetMasterInfo();
            apps_ = engine_.GetApps();
            for (auto& b : bars_) {
                if (b && b->barVisible) RenderBar(*b);
            }
            panel_.RefreshData();
            ShowTooltipForSlot(bar, idx, true);
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(bar, pt);
            if (idx < 0) return 0;

            const Slot& s = bar.slots[static_cast<size_t>(idx)];
            if (s.kind == SlotKind::Master || s.kind == SlotKind::Overflow) {
                HideTooltip(bar);
                panel_.Toggle(GetAnchorRectForBar(bar));
            } else if (s.kind == SlotKind::App && s.appIndex >= 0 && s.appIndex < static_cast<int>(apps_.size())) {
                const AppAudioInfo& app = apps_[static_cast<size_t>(s.appIndex)];
                engine_.ToggleAppMute(app.appId);
                master_ = engine_.GetMasterInfo();
                apps_ = engine_.GetApps();
                for (auto& b : bars_) {
                    if (b && b->barVisible) RenderBar(*b);
                }
                panel_.RefreshData();
                ShowTooltipForSlot(bar, idx, true);
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            HideTooltip(bar);
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(bar, pt);
            POINT scr = pt;
            ClientToScreen(bar.hwnd, &scr);

            auto cb = MakeMenuCallbacks();
            cb.onOpenPanel = [this, &bar]() {
                panel_.Toggle(GetAnchorRectForBar(bar));
            };
            if (idx >= 0 && bar.slots[static_cast<size_t>(idx)].kind == SlotKind::App) {
                int ai = bar.slots[static_cast<size_t>(idx)].appIndex;
                if (ai >= 0 && ai < static_cast<int>(apps_.size())) {
                    ui::ShowAppMenu(bar.hwnd, scr, apps_[static_cast<size_t>(ai)], engine_, config_, cb);
                    return 0;
                }
            }
            ui::ShowGeneralMenu(bar.hwnd, scr, engine_, config_, cb);
            return 0;
        }

        case WM_DPICHANGED:
            bar.dpi = HIWORD(wParam);
            if (bar.dpi == 0) bar.dpi = 96;
            UpdateBarLayoutAndPosition(bar);
            return 0;

        case WM_DISPLAYCHANGE:
            UpdateLayoutAndPosition();
            return 0;

        case WM_SETTINGCHANGE:
            UpdateTheme();
            UpdateLayoutAndPosition();
            return 0;
    }

    return DefWindowProcW(bar.hwnd, msg, wParam, lParam);
}
