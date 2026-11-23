#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif

namespace ui {

inline int ScaleDpi(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

inline bool IsSystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD val = 0;
    DWORD size = sizeof(val);
    bool dark = true;
    if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
        dark = (val == 0);
    }
    RegCloseKey(key);
    return dark;
}

struct Palette {
    bool dark = true;
    Gdiplus::Color accent{255, 96, 205, 255};
    Gdiplus::Color accentDim{255, 130, 142, 158};
    Gdiplus::Color mutedRed{255, 240, 108, 108};
    Gdiplus::Color peakGreen{255, 76, 208, 138};
    Gdiplus::Color textPrimary{255, 244, 246, 250};
    Gdiplus::Color textSecondary{190, 210, 218, 230};
    Gdiplus::Color trackBg{90, 255, 255, 255};
    Gdiplus::Color hoverFill{28, 255, 255, 255};
    Gdiplus::Color cardBg{32, 255, 255, 255};
    Gdiplus::Color panelTint{210, 24, 26, 32};
    Gdiplus::Color border{45, 255, 255, 255};
    Gdiplus::Color tooltipBg{242, 30, 32, 38};
    Gdiplus::Color tooltipBorder{70, 255, 255, 255};
};

inline Palette MakePalette(bool dark) {
    Palette p;
    p.dark = dark;
    if (!dark) {
        p.accent = Gdiplus::Color(255, 0, 103, 192);
        p.accentDim = Gdiplus::Color(255, 130, 138, 148);
        p.mutedRed = Gdiplus::Color(255, 200, 56, 56);
        p.peakGreen = Gdiplus::Color(255, 22, 160, 92);
        p.textPrimary = Gdiplus::Color(255, 24, 26, 30);
        p.textSecondary = Gdiplus::Color(185, 68, 74, 84);
        p.trackBg = Gdiplus::Color(60, 0, 0, 0);
        p.hoverFill = Gdiplus::Color(24, 0, 0, 0);
        p.cardBg = Gdiplus::Color(20, 0, 0, 0);
        p.panelTint = Gdiplus::Color(220, 244, 246, 250);
        p.border = Gdiplus::Color(40, 0, 0, 0);
        p.tooltipBg = Gdiplus::Color(245, 248, 250, 252);
        p.tooltipBorder = Gdiplus::Color(60, 0, 0, 0);
    }
    return p;
}

inline void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0.1f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

inline void FillRoundedRect(Gdiplus::Graphics& g, const Gdiplus::Brush& brush,
                            const Gdiplus::RectF& r, float radius) {
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, r, radius);
    g.FillPath(&brush, &path);
}

inline void DrawRoundedRect(Gdiplus::Graphics& g, const Gdiplus::Pen& pen,
                            const Gdiplus::RectF& r, float radius) {
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, r, radius);
    g.DrawPath(&pen, &path);
}

inline void DrawSpeakerGlyph(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                             float volume, bool muted, Gdiplus::Color fg, Gdiplus::Color muteColor) {
    float x = box.X;
    float y = box.Y;
    float w = box.Width;
    float h = box.Height;

    Gdiplus::PointF body[6] = {
        {x + w * 0.10f, y + h * 0.36f},
        {x + w * 0.28f, y + h * 0.36f},
        {x + w * 0.50f, y + h * 0.18f},
        {x + w * 0.50f, y + h * 0.82f},
        {x + w * 0.28f, y + h * 0.64f},
        {x + w * 0.10f, y + h * 0.64f}
    };
    Gdiplus::SolidBrush bodyBrush(muted ? muteColor : fg);
    g.FillPolygon(&bodyBrush, body, 6);

    float stroke = (w * 0.085f < 1.3f) ? 1.3f : (w * 0.085f);
    if (muted || volume <= 0.001f) {
        Gdiplus::Pen xp(muteColor, stroke);
        xp.SetStartCap(Gdiplus::LineCapRound);
        xp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&xp, x + w * 0.62f, y + h * 0.34f, x + w * 0.90f, y + h * 0.66f);
        g.DrawLine(&xp, x + w * 0.90f, y + h * 0.34f, x + w * 0.62f, y + h * 0.66f);
        return;
    }

    Gdiplus::Pen wavePen(fg, stroke);
    wavePen.SetStartCap(Gdiplus::LineCapRound);
    wavePen.SetEndCap(Gdiplus::LineCapRound);

    if (volume > 0.01f) {
        Gdiplus::RectF arc1(x + w * 0.44f, y + h * 0.31f, w * 0.24f, h * 0.38f);
        g.DrawArc(&wavePen, arc1, -55.0f, 110.0f);
    }
    if (volume >= 0.45f) {
        Gdiplus::RectF arc2(x + w * 0.48f, y + h * 0.18f, w * 0.40f, h * 0.64f);
        g.DrawArc(&wavePen, arc2, -55.0f, 110.0f);
    }
}

} // namespace ui
