#include "ui/TaskbarWidget.h"

#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

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
           lstrcmpiW(cls, TaskbarWidget::kWindowClass) == 0;
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
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW twc{};
    twc.cbSize = sizeof(twc);
    twc.lpfnWndProc = TooltipWndProc;
    twc.hInstance = hInst;
    twc.lpszClassName = kTooltipClass;
    RegisterClassExW(&twc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kWindowClass, L"VolumeBar",
        WS_POPUP,
        0, 0, 100, 40,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    tooltipHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kTooltipClass, L"",
        WS_POPUP,
        0, 0, 10, 10,
        nullptr, nullptr, hInst, nullptr);

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;

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
    return true;
}

void TaskbarWidget::Destroy() {
    gActiveWidget = nullptr;
    if (hookForeground_) { UnhookWinEvent(hookForeground_); hookForeground_ = nullptr; }
    if (hookReorder_)    { UnhookWinEvent(hookReorder_);    hookReorder_ = nullptr; }
    if (hookMinimize_)   { UnhookWinEvent(hookMinimize_);   hookMinimize_ = nullptr; }

    UnregisterHotkeys();

    if (tooltipHwnd_) {
        DestroyWindow(tooltipHwnd_);
        tooltipHwnd_ = nullptr;
    }
    if (hwnd_) {
        KillTimer(hwnd_, kDirtyDebounceTimerId);
        KillTimer(hwnd_, kTooltipHideTimerId);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    barVisible_ = false;
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

RECT TaskbarWidget::GetAnchorRect() const {
    if (barVisible_ && (barRect_.right > barRect_.left)) {
        return barRect_;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(mon, &mi)) {
        return {mi.rcWork.left + 8, mi.rcWork.bottom, mi.rcWork.left + 200, mi.rcWork.bottom + 48};
    }
    return {8, 1032, 200, 1080};
}

bool TaskbarWidget::QueryTaskbarRect(RECT* outRect, bool* outHidden) const {
    if (!outRect || !outHidden) return false;
    *outHidden = false;

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        *outHidden = true;
        return false;
    }

    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = tray;

    UINT_PTR state = SHAppBarMessage(ABM_GETSTATE, &abd);
    bool autoHide = (state & ABS_AUTOHIDE) != 0;

    RECT rc{};
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd) != 0) {
        rc = abd.rc;
    } else if (!GetWindowRect(tray, &rc)) {
        *outHidden = true;
        return false;
    }

    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        *outHidden = true;
        return false;
    }

    if (autoHide) {
        RECT trayRc{};
        if (GetWindowRect(tray, &trayRc)) {
            HMONITOR mon = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                int visH = (std::min)(trayRc.bottom, mi.rcMonitor.bottom) - (std::max)(trayRc.top, mi.rcMonitor.top);
                if (visH < 12) {
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

bool TaskbarWidget::IsFullscreenAppActive() const {
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        if (quns == QUNS_RUNNING_D3D_FULL_SCREEN ||
            quns == QUNS_BUSY ||
            quns == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }

    HWND fg = GetForegroundWindow();
    if (!fg || IsIgnoredFullscreenClass(fg)) return false;

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;

    HMONITOR trayMon = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (trayMon != fgMon) return false;

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
    if (barVisible_) Render();
    panel_.UpdateTheme();
}

void TaskbarWidget::BuildSlots() {
    slots_.clear();
    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

    int barH = barRect_.bottom - barRect_.top;
    int slotW = sc(34);
    int padX = sc(4);
    int maxW = sc(config_.Data().maxWidth);
    if (maxW < slotW * 2) maxW = slotW * 2;

    int x = padX;

    // Slot 0: Master
    Slot mSlot;
    mSlot.kind = SlotKind::Master;
    mSlot.rect = {x, 0, x + slotW, barH};
    slots_.push_back(mSlot);
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
            slots_.push_back(ov);
            x += sc(28);
            break;
        }
        Slot s;
        s.kind = SlotKind::App;
        s.appIndex = i;
        s.rect = {x, 0, x + slotW, barH};
        slots_.push_back(s);
        x += slotW;
    }

    int finalW = (std::min)(maxW, x + padX);
    barRect_.right = barRect_.left + finalW;
}

void TaskbarWidget::UpdateLayoutAndPosition() {
    if (!hwnd_) return;

    RECT trayRc{};
    bool hidden = false;
    if (!QueryTaskbarRect(&trayRc, &hidden) || hidden || IsFullscreenAppActive()) {
        if (barVisible_) {
            ShowWindow(hwnd_, SW_HIDE);
            barVisible_ = false;
            HideTooltip();
        }
        return;
    }

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        UINT trayDpi = GetDpiForWindow(tray);
        if (trayDpi != 0 && trayDpi != dpi_) {
            dpi_ = trayDpi;
            icons_.Clear();
        }
    }

    int leftMargin = ui::ScaleDpi(8, dpi_);
    barRect_.left = trayRc.left + leftMargin;
    barRect_.top = trayRc.top;
    barRect_.bottom = trayRc.bottom;
    barRect_.right = barRect_.left + ui::ScaleDpi(config_.Data().maxWidth, dpi_);

    BuildSlots();

    int w = barRect_.right - barRect_.left;
    int h = barRect_.bottom - barRect_.top;

    SetWindowPos(hwnd_, HWND_TOPMOST, barRect_.left, barRect_.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    barVisible_ = true;
    Render();
}

void TaskbarWidget::CheckVisibilityAndZOrder() {
    if (!hwnd_) return;

    RECT trayRc{};
    bool hidden = false;
    if (!QueryTaskbarRect(&trayRc, &hidden) || hidden || IsFullscreenAppActive()) {
        if (barVisible_) {
            ShowWindow(hwnd_, SW_HIDE);
            barVisible_ = false;
            HideTooltip();
        }
        return;
    }

    if (!barVisible_) {
        UpdateLayoutAndPosition();
    } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
}

void TaskbarWidget::Render() {
    if (!hwnd_ || !barVisible_) return;

    int w = barRect_.right - barRect_.left;
    int h = barRect_.bottom - barRect_.top;
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
    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

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

        for (size_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            int sw = s.rect.right - s.rect.left;

            // Hover pill
            if (static_cast<int>(i) == hoveredSlot_) {
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
                        // Discreet dimmed icon for paused/inactive apps (user preference)
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
    POINT ptDst = {barRect_.left, barRect_.top};
    SIZE sz = {w, h};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

int TaskbarWidget::HitTestSlot(POINT pt) const {
    for (size_t i = 0; i < slots_.size(); ++i) {
        const RECT& r = slots_[i].rect;
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TaskbarWidget::ShowTooltipForSlot(int slotIdx, bool autoHideAfterScroll) {
    if (slotIdx < 0 || slotIdx >= static_cast<int>(slots_.size())) {
        HideTooltip();
        return;
    }

    const Slot& s = slots_[static_cast<size_t>(slotIdx)];
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

    POINT anchor = {barRect_.left + (s.rect.left + s.rect.right) / 2, barRect_.top};
    ShowTooltipText(text, anchor, autoHideAfterScroll);
}

void TaskbarWidget::ShowTooltipText(const std::wstring& text, POINT anchorScreenPt, bool autoHide) {
    if (!tooltipHwnd_ || text.empty()) return;
    RenderTooltip(text, anchorScreenPt);
    if (autoHide) {
        SetTimer(hwnd_, kTooltipHideTimerId, 1300, nullptr);
    } else {
        KillTimer(hwnd_, kTooltipHideTimerId);
    }
}

void TaskbarWidget::HideTooltip() {
    if (hwnd_) KillTimer(hwnd_, kTooltipHideTimerId);
    if (tooltipHwnd_) ShowWindow(tooltipHwnd_, SW_HIDE);
}

void TaskbarWidget::RenderTooltip(const std::wstring& text, POINT anchorScreenPt) {
    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

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
    if (y < mi.rcMonitor.top + sc(4)) y = barRect_.bottom + sc(8);

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

        SetWindowPos(tooltipHwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateLayeredWindow(tooltipHwnd_, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);

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
            if (wParam == kTooltipHideTimerId) {
                HideTooltip();
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
                    POINT anchor = {barRect_.left + (barRect_.right - barRect_.left) / 2, barRect_.top};
                    ShowTooltipText(L"Perfil: " + profName, anchor, true);
                }
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                trackingMouse_ = true;
            }
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(pt);
            if (idx != hoveredSlot_) {
                hoveredSlot_ = idx;
                Render();
                if (idx >= 0) ShowTooltipForSlot(idx, false);
                else HideTooltip();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (hoveredSlot_ != -1) {
                hoveredSlot_ = -1;
                Render();
            }
            HideTooltip();
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            int idx = HitTestSlot(pt);
            if (idx < 0) return 0;

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
            float step = static_cast<float>(shift ? 10 : config_.Data().scrollStep) / 100.0f;
            float dir = (delta > 0) ? 1.0f : -1.0f;

            const Slot& s = slots_[static_cast<size_t>(idx)];
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
            Render();
            panel_.RefreshData();
            ShowTooltipForSlot(idx, true);
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(pt);
            if (idx < 0) return 0;

            const Slot& s = slots_[static_cast<size_t>(idx)];
            if (s.kind == SlotKind::Master || s.kind == SlotKind::Overflow) {
                HideTooltip();
                panel_.Toggle(GetAnchorRect());
            } else if (s.kind == SlotKind::App && s.appIndex >= 0 && s.appIndex < static_cast<int>(apps_.size())) {
                const AppAudioInfo& app = apps_[static_cast<size_t>(s.appIndex)];
                engine_.ToggleAppMute(app.appId);
                master_ = engine_.GetMasterInfo();
                apps_ = engine_.GetApps();
                Render();
                panel_.RefreshData();
                ShowTooltipForSlot(idx, true);
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            HideTooltip();
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int idx = HitTestSlot(pt);
            POINT scr = pt;
            ClientToScreen(hwnd_, &scr);

            auto cb = MakeMenuCallbacks();
            if (idx >= 0 && slots_[static_cast<size_t>(idx)].kind == SlotKind::App) {
                int ai = slots_[static_cast<size_t>(idx)].appIndex;
                if (ai >= 0 && ai < static_cast<int>(apps_.size())) {
                    ui::ShowAppMenu(hwnd_, scr, apps_[static_cast<size_t>(ai)], engine_, config_, cb);
                    return 0;
                }
            }
            ui::ShowGeneralMenu(hwnd_, scr, engine_, config_, cb);
            return 0;
        }

        case WM_DPICHANGED:
            dpi_ = HIWORD(wParam);
            if (dpi_ == 0) dpi_ = 96;
            icons_.Clear();
            UpdateLayoutAndPosition();
            return 0;

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
