#include "ui/MixerPanel.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr wchar_t kPanelClass[] = L"VolumeBar_MixerPanel";
constexpr UINT_PTR kPeakTimerId = 1;
constexpr UINT kPeakTimerMs = 33; // ~30 fps only while panel is visible

inline int ToPct(float v) {
    int p = static_cast<int>(std::lround(v * 100.0f));
    return (std::max)(0, (std::min)(100, p));
}

inline bool PtInRc(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

UINT QueryMonitorDpi(HMONITOR mon, UINT fallback) {
    if (!mon) return fallback;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = GetModuleHandleW(L"Shcore.dll");
    if (!shcore) shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        if (fn) {
            UINT dpiX = 0;
            UINT dpiY = 0;
            // MDT_EFFECTIVE_DPI = 0
            if (SUCCEEDED(fn(mon, 0, &dpiX, &dpiY)) && dpiX > 0) {
                return dpiX;
            }
        }
    }
    return fallback;
}

} // namespace

MixerPanel::MixerPanel(AudioEngine& engine, ConfigManager& config, AppIconCache& icons)
    : engine_(engine), config_(config), icons_(icons) {
    palette_ = ui::MakePalette(ui::IsSystemDarkMode());
}

MixerPanel::~MixerPanel() {
    Destroy();
}

bool MixerPanel::Create(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kPanelClass, L"VolumeBar",
        WS_POPUP,
        0, 0, 380, 260,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;
    ApplyDwmStyle();
    return true;
}

void MixerPanel::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kPeakTimerId);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

void MixerPanel::ApplyDwmStyle() {
    if (!hwnd_) return;
    BOOL dark = palette_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    DWORD backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
}

void MixerPanel::UpdateTheme() {
    palette_ = ui::MakePalette(ui::IsSystemDarkMode());
    ApplyDwmStyle();
    if (visible_ && hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MixerPanel::Show(const RECT& anchorRect) {
    if (!hwnd_) return;
    lastAnchor_ = anchorRect;
    scrollY_ = 0;
    visible_ = true;
    RefreshData();
    RecomputeSizeAndPosition();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kPeakTimerId, kPeakTimerMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MixerPanel::Hide() {
    if (!hwnd_) return;
    visible_ = false;
    dragging_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    KillTimer(hwnd_, kPeakTimerId);
    ShowWindow(hwnd_, SW_HIDE);
}

void MixerPanel::Toggle(const RECT& anchorRect) {
    if (visible_) Hide();
    else Show(anchorRect);
}

void MixerPanel::RefreshData() {
    master_ = engine_.GetMasterInfo();
    apps_ = engine_.GetApps();
    devices_ = engine_.GetDevices();

    if (visible_ && hwnd_) {
        RecomputeSizeAndPosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MixerPanel::RecomputeSizeAndPosition() {
    if (!hwnd_) return;

    POINT refPt = {lastAnchor_.left, lastAnchor_.top};
    HMONITOR mon = MonitorFromPoint(refPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    UINT monDpi = QueryMonitorDpi(mon, dpi_);
    if (monDpi != 0) dpi_ = monDpi;

    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

    int panelW = sc(388);
    int headerH = sc(44);
    int masterH = sc(78);
    int rowH = sc(72);
    int bottomPad = sc(14);
    int emptyH = sc(46);

    int listContentH = apps_.empty() ? emptyH : static_cast<int>(apps_.size()) * rowH;
    int desiredH = headerH + masterH + listContentH + bottomPad;
    int maxH = sc(540);
    int panelH = (std::min)(desiredH, maxH);

    listTopY_ = headerH + masterH;
    contentHeight_ = listContentH;
    int visibleListH = panelH - listTopY_ - bottomPad;
    int maxScroll = (std::max)(0, contentHeight_ - visibleListH);
    scrollY_ = (std::max)(0, (std::min)(scrollY_, maxScroll));

    int gap = sc(10);
    int x = lastAnchor_.left;
    int y = lastAnchor_.top - panelH - gap;

    if (y < mi.rcWork.top + gap) {
        y = lastAnchor_.bottom + gap;
    }
    if (x + panelW > mi.rcWork.right - gap) {
        x = mi.rcWork.right - panelW - gap;
    }
    if (x < mi.rcWork.left + gap) {
        x = mi.rcWork.left + gap;
    }
    if (y + panelH > mi.rcWork.bottom - gap) {
        y = mi.rcWork.bottom - panelH - gap;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, panelW, panelH,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
    BuildLayout();
}

void MixerPanel::BuildLayout() {
    rows_.clear();
    if (!hwnd_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

    int pad = sc(14);
    profileBtnRect_ = {w - pad - sc(74), sc(10), w - pad, sc(34)};

    // Master row
    {
        RowLayout r;
        r.isMaster = true;
        int top = sc(44);
        int h = sc(68);
        r.rowRect = {pad, top, w - pad, top + h};
        r.muteRect = {r.rowRect.left + sc(10), top + sc(20), r.rowRect.left + sc(38), top + sc(48)};
        r.sliderRect = {r.rowRect.left + sc(48), top + sc(34), r.rowRect.right - sc(54), top + sc(54)};
        rows_.push_back(std::move(r));
    }

    // App rows
    int rowH = sc(72);
    int y = listTopY_ - scrollY_;
    for (const auto& app : apps_) {
        RowLayout r;
        r.isMaster = false;
        r.appId = app.appId;
        r.rowRect = {pad, y, w - pad, y + rowH - sc(6)};
        r.muteRect = {r.rowRect.left + sc(10), y + sc(34), r.rowRect.left + sc(36), y + sc(58)};
        r.sliderRect = {r.rowRect.left + sc(44), y + sc(34), r.rowRect.right - sc(54), y + sc(54)};
        r.deviceRect = {r.rowRect.right - sc(124), y + sc(7), r.rowRect.right - sc(8), y + sc(29)};
        rows_.push_back(std::move(r));
        y += rowH;
    }
}

void MixerPanel::UpdatePeaks() {
    float rawMaster = engine_.GetMasterPeak();
    masterPeak_ = (std::max)(rawMaster, masterPeak_ * 0.80f);
    if (masterPeak_ < 0.005f) masterPeak_ = 0.0f;

    for (const auto& app : apps_) {
        float raw = engine_.GetAppPeak(app.appId);
        float prev = appPeaks_[app.appId];
        float smooth = (std::max)(raw, prev * 0.80f);
        if (smooth < 0.005f) smooth = 0.0f;
        appPeaks_[app.appId] = smooth;
    }
    if (visible_ && hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

std::wstring MixerPanel::DeviceShortName(const AppAudioInfo& app) const {
    if (app.isSystemSounds || !engine_.IsPerAppRoutingAvailable()) {
        return L"Padrão";
    }
    if (app.persistedDeviceId.empty()) {
        return L"Padrão ▾";
    }
    for (const auto& d : devices_) {
        if (d.id == app.persistedDeviceId) {
            std::wstring n = d.name;
            if (n.size() > 14) n = n.substr(0, 13) + L"…";
            return n + L" ▾";
        }
    }
    return L"Padrão ▾";
}

void MixerPanel::Render(HDC paintDc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(paintDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return;

    HDC memDc = CreateCompatibleDC(paintDc);
    HGDIOBJ oldBmp = SelectObject(memDc, dib);

    auto sc = [this](int dip) { return ui::ScaleDpi(dip, dpi_); };

    {
        Gdiplus::Graphics g(memDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        // Clear to translucent tint so Win11 DWM Acrylic shows through cleanly
        g.Clear(palette_.panelTint);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font titleFont(&ff, static_cast<Gdiplus::REAL>(sc(14)), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font labelFont(&ff, static_cast<Gdiplus::REAL>(sc(13)), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font smallFont(&ff, static_cast<Gdiplus::REAL>(sc(11)), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::SolidBrush textPrimary(palette_.textPrimary);
        Gdiplus::SolidBrush textSecondary(palette_.textSecondary);
        Gdiplus::SolidBrush cardBrush(palette_.cardBg);
        Gdiplus::Pen borderPen(palette_.border, 1.0f);

        Gdiplus::StringFormat leftSf;
        leftSf.SetAlignment(Gdiplus::StringAlignmentNear);
        leftSf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        leftSf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        leftSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::StringFormat rightSf;
        rightSf.SetAlignment(Gdiplus::StringAlignmentFar);
        rightSf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        rightSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::StringFormat centerSf;
        centerSf.SetAlignment(Gdiplus::StringAlignmentCenter);
        centerSf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        centerSf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        centerSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        // Header
        int pad = sc(14);
        Gdiplus::RectF hdrRc(static_cast<float>(pad), static_cast<float>(sc(8)),
                             static_cast<float>(w - pad * 2 - sc(82)), static_cast<float>(sc(28)));
        g.DrawString(L"VolumeBar", -1, &titleFont, hdrRc, &leftSf, &textPrimary);

        // Profiles button
        Gdiplus::RectF profRc(static_cast<float>(profileBtnRect_.left), static_cast<float>(profileBtnRect_.top),
                              static_cast<float>(profileBtnRect_.right - profileBtnRect_.left),
                              static_cast<float>(profileBtnRect_.bottom - profileBtnRect_.top));
        ui::FillRoundedRect(g, cardBrush, profRc, static_cast<float>(sc(6)));
        ui::DrawRoundedRect(g, borderPen, profRc, static_cast<float>(sc(6)));
        g.DrawString(L"Perfis ▾", -1, &smallFont, profRc, &centerSf, &textPrimary);

        auto drawSliderAndPeak = [&](const RECT& sr, float volume, bool muted, bool active, float peak) {
            float sx = static_cast<float>(sr.left);
            float sw = static_cast<float>(sr.right - sr.left);
            float cy = static_cast<float>(sr.top + sr.bottom) * 0.5f;
            float trackH = static_cast<float>(sc(4));
            float trackY = cy - trackH * 0.5f;

            Gdiplus::SolidBrush trackBrush(palette_.trackBg);
            ui::FillRoundedRect(g, trackBrush, Gdiplus::RectF(sx, trackY, sw, trackH), trackH * 0.5f);

            float fillW = sw * (std::max)(0.0f, (std::min)(1.0f, volume));
            Gdiplus::Color fillCol = muted ? palette_.mutedRed : (active ? palette_.accent : palette_.accentDim);
            if (fillW > 1.0f) {
                Gdiplus::SolidBrush fillBrush(fillCol);
                ui::FillRoundedRect(g, fillBrush, Gdiplus::RectF(sx, trackY, fillW, trackH), trackH * 0.5f);
            }

            // Thumb
            float thumbR = static_cast<float>(sc(6));
            float thumbX = sx + fillW - thumbR;
            float thumbY = cy - thumbR;
            Gdiplus::SolidBrush thumbOuter(palette_.textPrimary);
            Gdiplus::SolidBrush thumbInner(fillCol);
            g.FillEllipse(&thumbOuter, thumbX, thumbY, thumbR * 2.0f, thumbR * 2.0f);
            g.FillEllipse(&thumbInner, thumbX + 2.0f, thumbY + 2.0f, (thumbR - 2.0f) * 2.0f, (thumbR - 2.0f) * 2.0f);

            // Peak meter bar right under slider
            float peakH = static_cast<float>(sc(3));
            float peakY = static_cast<float>(sr.bottom) + static_cast<float>(sc(2));
            Gdiplus::SolidBrush peakTrack(Gdiplus::Color(40, 255, 255, 255));
            ui::FillRoundedRect(g, peakTrack, Gdiplus::RectF(sx, peakY, sw, peakH), peakH * 0.5f);
            if (peak > 0.005f && !muted) {
                float pw = sw * (std::max)(0.0f, (std::min)(1.0f, peak));
                Gdiplus::SolidBrush peakBrush(palette_.peakGreen);
                ui::FillRoundedRect(g, peakBrush, Gdiplus::RectF(sx, peakY, pw, peakH), peakH * 0.5f);
            }
        };

        // Master card
        if (!rows_.empty()) {
            const RowLayout& mr = rows_[0];
            Gdiplus::RectF cardRc(static_cast<float>(mr.rowRect.left), static_cast<float>(mr.rowRect.top),
                                  static_cast<float>(mr.rowRect.right - mr.rowRect.left),
                                  static_cast<float>(mr.rowRect.bottom - mr.rowRect.top));
            ui::FillRoundedRect(g, cardBrush, cardRc, static_cast<float>(sc(8)));
            ui::DrawRoundedRect(g, borderPen, cardRc, static_cast<float>(sc(8)));

            Gdiplus::RectF spkRc(static_cast<float>(mr.muteRect.left + sc(3)), static_cast<float>(mr.muteRect.top + sc(3)),
                                 static_cast<float>(sc(22)), static_cast<float>(sc(22)));
            ui::DrawSpeakerGlyph(g, spkRc, master_.volume, master_.muted, palette_.textPrimary, palette_.mutedRed);

            std::wstring masterTitle = L"Mestre";
            if (!master_.defaultDeviceName.empty()) {
                masterTitle += L" — " + master_.defaultDeviceName;
            }
            Gdiplus::RectF mLbl(static_cast<float>(mr.sliderRect.left), static_cast<float>(mr.rowRect.top + sc(8)),
                                static_cast<float>(mr.rowRect.right - mr.sliderRect.left - sc(12)), static_cast<float>(sc(20)));
            g.DrawString(masterTitle.c_str(), -1, &labelFont, mLbl, &leftSf, &textPrimary);

            drawSliderAndPeak(mr.sliderRect, master_.volume, master_.muted, true, masterPeak_);

            std::wstring pctStr = master_.muted ? L"Mudo" : (std::to_wstring(ToPct(master_.volume)) + L"%");
            Gdiplus::RectF pctRc(static_cast<float>(mr.sliderRect.right + sc(4)), static_cast<float>(mr.sliderRect.top),
                                 static_cast<float>(sc(44)), static_cast<float>(mr.sliderRect.bottom - mr.sliderRect.top));
            g.DrawString(pctStr.c_str(), -1, &smallFont, pctRc, &rightSf, &textSecondary);
        }

        // Clip scrollable app area
        Gdiplus::Rect clipRc(0, listTopY_, w, h - listTopY_ - sc(10));
        g.SetClip(clipRc);

        if (apps_.empty()) {
            Gdiplus::RectF emptyRc(static_cast<float>(pad), static_cast<float>(listTopY_ + sc(8)),
                                   static_cast<float>(w - pad * 2), static_cast<float>(sc(32)));
            g.DrawString(L"Nenhum aplicativo com áudio ativo.", -1, &labelFont, emptyRc, &centerSf, &textSecondary);
        } else {
            int iconPx = sc(20);
            for (size_t i = 0; i < apps_.size() && (i + 1) < rows_.size(); ++i) {
                const AppAudioInfo& app = apps_[i];
                const RowLayout& r = rows_[i + 1];
                if (r.rowRect.bottom < listTopY_ || r.rowRect.top > h) continue;

                Gdiplus::RectF cardRc(static_cast<float>(r.rowRect.left), static_cast<float>(r.rowRect.top),
                                      static_cast<float>(r.rowRect.right - r.rowRect.left),
                                      static_cast<float>(r.rowRect.bottom - r.rowRect.top));
                Gdiplus::SolidBrush rowBg(app.isActive ? palette_.cardBg : Gdiplus::Color(16, 255, 255, 255));
                ui::FillRoundedRect(g, rowBg, cardRc, static_cast<float>(sc(8)));

                // App icon
                Gdiplus::Bitmap* bmp = icons_.GetIcon(app, iconPx);
                if (bmp) {
                    float ix = static_cast<float>(r.rowRect.left + sc(10));
                    float iy = static_cast<float>(r.rowRect.top + sc(8));
                    if (app.isActive && !app.muted) {
                        g.DrawImage(bmp, Gdiplus::RectF(ix, iy, static_cast<float>(iconPx), static_cast<float>(iconPx)));
                    } else {
                        Gdiplus::ColorMatrix cm = {{
                            {1, 0, 0, 0, 0},
                            {0, 1, 0, 0, 0},
                            {0, 0, 1, 0, 0},
                            {0, 0, 0, app.muted ? 0.42f : 0.52f, 0},
                            {0, 0, 0, 0, 1}
                        }};
                        Gdiplus::ImageAttributes ia;
                        ia.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                        g.DrawImage(bmp, Gdiplus::RectF(ix, iy, static_cast<float>(iconPx), static_cast<float>(iconPx)),
                                    0, 0, static_cast<Gdiplus::REAL>(bmp->GetWidth()), static_cast<Gdiplus::REAL>(bmp->GetHeight()),
                                    Gdiplus::UnitPixel, &ia);
                    }
                }

                // App name (+ Pausado tag when inactive)
                std::wstring title = app.displayName;
                if (!app.isActive) title += L" · Pausado";
                Gdiplus::RectF nameRc(static_cast<float>(r.rowRect.left + sc(38)),
                                      static_cast<float>(r.rowRect.top + sc(7)),
                                      static_cast<float>(r.deviceRect.left - (r.rowRect.left + sc(44))),
                                      static_cast<float>(sc(22)));
                g.DrawString(title.c_str(), -1, &labelFont, nameRc, &leftSf,
                             app.isActive ? &textPrimary : &textSecondary);

                // Per-app output device button
                bool devEnabled = engine_.IsPerAppRoutingAvailable() && !app.isSystemSounds;
                Gdiplus::RectF devRc(static_cast<float>(r.deviceRect.left), static_cast<float>(r.deviceRect.top),
                                     static_cast<float>(r.deviceRect.right - r.deviceRect.left),
                                     static_cast<float>(r.deviceRect.bottom - r.deviceRect.top));
                if (devEnabled) {
                    Gdiplus::SolidBrush devBg(Gdiplus::Color(26, 255, 255, 255));
                    ui::FillRoundedRect(g, devBg, devRc, static_cast<float>(sc(5)));
                    ui::DrawRoundedRect(g, borderPen, devRc, static_cast<float>(sc(5)));
                }
                std::wstring devLabel = DeviceShortName(app);
                Gdiplus::SolidBrush textDisabled(Gdiplus::Color(90, 180, 185, 195));
                g.DrawString(devLabel.c_str(), -1, &smallFont, devRc, &centerSf,
                             devEnabled ? &textSecondary : &textDisabled);

                // Mute button
                Gdiplus::RectF mRc(static_cast<float>(r.muteRect.left + sc(3)), static_cast<float>(r.muteRect.top + sc(3)),
                                   static_cast<float>(sc(18)), static_cast<float>(sc(18)));
                ui::DrawSpeakerGlyph(g, mRc, app.volume, app.muted,
                                     app.isActive ? palette_.textPrimary : palette_.textSecondary,
                                     palette_.mutedRed);

                // Slider + Peak
                float peak = 0.0f;
                auto pit = appPeaks_.find(app.appId);
                if (pit != appPeaks_.end()) peak = pit->second;
                drawSliderAndPeak(r.sliderRect, app.volume, app.muted, app.isActive, peak);

                std::wstring pctStr = app.muted ? L"Mudo" : (std::to_wstring(ToPct(app.volume)) + L"%");
                Gdiplus::RectF pctRc(static_cast<float>(r.sliderRect.right + sc(4)), static_cast<float>(r.sliderRect.top),
                                     static_cast<float>(sc(44)), static_cast<float>(r.sliderRect.bottom - r.sliderRect.top));
                g.DrawString(pctStr.c_str(), -1, &smallFont, pctRc, &rightSf, &textSecondary);
            }
        }
    }

    BitBlt(paintDc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    DeleteObject(dib);
}

float MixerPanel::SliderRatioFromX(const RECT& sliderRect, int x) const {
    int span = sliderRect.right - sliderRect.left;
    if (span <= 0) return 0.0f;
    float r = static_cast<float>(x - sliderRect.left) / static_cast<float>(span);
    return (std::max)(0.0f, (std::min)(1.0f, r));
}

void MixerPanel::OnMouseDown(POINT pt) {
    if (PtInRc(profileBtnRect_, pt)) {
        suppressDeactivate_ = true;
        POINT scr = {profileBtnRect_.left, profileBtnRect_.bottom};
        ClientToScreen(hwnd_, &scr);
        ui::ShowGeneralMenu(hwnd_, scr, engine_, config_, callbacks_);
        suppressDeactivate_ = false;
        RefreshData();
        return;
    }

    for (size_t i = 0; i < rows_.size(); ++i) {
        const RowLayout& r = rows_[i];
        if (!r.isMaster && pt.y < listTopY_) continue;

        if (PtInRc(r.muteRect, pt)) {
            if (r.isMaster) engine_.ToggleMasterMute();
            else engine_.ToggleAppMute(r.appId);
            if (callbacks_.onStateChanged) callbacks_.onStateChanged();
            RefreshData();
            return;
        }

        if (PtInRc(r.sliderRect, pt)) {
            dragging_ = true;
            dragIsMaster_ = r.isMaster;
            dragAppId_ = r.appId;
            dragSliderRect_ = r.sliderRect;
            SetCapture(hwnd_);

            float ratio = SliderRatioFromX(r.sliderRect, pt.x);
            if (r.isMaster) engine_.SetMasterVolume(ratio);
            else engine_.SetAppVolume(r.appId, ratio);
            if (callbacks_.onStateChanged) callbacks_.onStateChanged();
            RefreshData();
            return;
        }

        if (!r.isMaster && PtInRc(r.deviceRect, pt) && (i - 1) < apps_.size()) {
            const AppAudioInfo& app = apps_[i - 1];
            if (engine_.IsPerAppRoutingAvailable() && !app.isSystemSounds) {
                suppressDeactivate_ = true;
                POINT scr = {r.deviceRect.left, r.deviceRect.bottom};
                ClientToScreen(hwnd_, &scr);
                ui::ShowDevicePickerMenu(hwnd_, scr, app, engine_, [this]() {
                    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
                    RefreshData();
                });
                suppressDeactivate_ = false;
            }
            return;
        }
    }
}

void MixerPanel::OnMouseMove(POINT pt) {
    if (!dragging_) return;
    float ratio = SliderRatioFromX(dragSliderRect_, pt.x);
    if (dragIsMaster_) {
        engine_.SetMasterVolume(ratio);
        master_.volume = ratio;
    } else {
        engine_.SetAppVolume(dragAppId_, ratio);
        for (auto& a : apps_) {
            if (a.appId == dragAppId_) {
                a.volume = ratio;
                break;
            }
        }
    }
    if (callbacks_.onStateChanged) callbacks_.onStateChanged();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MixerPanel::OnMouseUp() {
    if (dragging_) {
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
}

void MixerPanel::OnMouseWheel(POINT pt, int delta, bool shift) {
    // Check if scrolling over a slider row -> adjust volume
    float step = static_cast<float>(shift ? 10 : config_.Data().scrollStep) / 100.0f;
    float dir = (delta > 0) ? 1.0f : -1.0f;

    for (const auto& r : rows_) {
        if (!r.isMaster && pt.y < listTopY_) continue;
        if (PtInRc(r.sliderRect, pt)) {
            if (r.isMaster) {
                engine_.SetMasterVolume(master_.volume + dir * step);
            } else {
                for (const auto& a : apps_) {
                    if (a.appId == r.appId) {
                        engine_.SetAppVolume(a.appId, a.volume + dir * step);
                        break;
                    }
                }
            }
            if (callbacks_.onStateChanged) callbacks_.onStateChanged();
            RefreshData();
            return;
        }
    }

    // Otherwise scroll app list
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int visibleListH = (rc.bottom - rc.top) - listTopY_ - ui::ScaleDpi(14, dpi_);
    int maxScroll = (std::max)(0, contentHeight_ - visibleListH);
    if (maxScroll > 0) {
        scrollY_ -= (delta / WHEEL_DELTA) * ui::ScaleDpi(36, dpi_);
        scrollY_ = (std::max)(0, (std::min)(scrollY_, maxScroll));
        BuildLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK MixerPanel::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MixerPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MixerPanel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MixerPanel::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            Render(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kPeakTimerId) {
                UpdatePeaks();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            OnMouseDown({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_LBUTTONUP:
            OnMouseUp();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
            OnMouseWheel(pt, GET_WHEEL_DELTA_WPARAM(wParam), shift);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                Hide();
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !suppressDeactivate_) {
                Hide();
            }
            return 0;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wParam);
            if (dpi_ == 0) dpi_ = 96;
            RecomputeSizeAndPosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
