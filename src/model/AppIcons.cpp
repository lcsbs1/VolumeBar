#include "model/AppIcons.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "util/Strings.h"

AppIconCache::~AppIconCache() {
    Clear();
}

void AppIconCache::Clear() {
    cache_.clear();
}

Gdiplus::Bitmap* AppIconCache::GetIcon(const AppAudioInfo& app, int targetPx) {
    if (targetPx < 12) targetPx = 16;

    std::wstring cacheKey = app.appId + L"|" + app.exePath + L"|" + app.iconPath + L"|" + std::to_wstring(targetPx);
    auto it = cache_.find(cacheKey);
    if (it != cache_.end()) {
        return it->second.get();
    }

    std::unique_ptr<Gdiplus::Bitmap> bmp;

    if (app.isSystemSounds) {
        HICON hIcon = LoadIconFromPathSpec(L"%SystemRoot%\\System32\\imageres.dll,-110");
        if (!hIcon) hIcon = LoadIconFromPathSpec(L"%SystemRoot%\\System32\\ddores.dll,-3012");
        if (hIcon) {
            bmp = CreateBitmapFromHicon(hIcon, targetPx);
            DestroyIcon(hIcon);
        }
    }

    if (!bmp && !app.iconPath.empty()) {
        HICON hIcon = LoadIconFromPathSpec(app.iconPath);
        if (hIcon) {
            bmp = CreateBitmapFromHicon(hIcon, targetPx);
            DestroyIcon(hIcon);
        }
    }

    if (!bmp && !app.exePath.empty()) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(app.exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
            bmp = CreateBitmapFromHicon(sfi.hIcon, targetPx);
            DestroyIcon(sfi.hIcon);
        }
    }

    if (!bmp) {
        bmp = CreateFallbackBadge(app, targetPx);
    }

    Gdiplus::Bitmap* raw = bmp.get();
    cache_[cacheKey] = std::move(bmp);
    return raw;
}

HICON AppIconCache::LoadIconFromPathSpec(const std::wstring& iconPath) {
    if (iconPath.empty()) return nullptr;

    std::wstring spec = iconPath;
    if (!spec.empty() && spec[0] == L'@') {
        spec.erase(0, 1);
    }

    std::wstring filePart = spec;
    int iconIndex = 0;
    size_t comma = spec.find_last_of(L',');
    if (comma != std::wstring::npos) {
        filePart = util::Trim(spec.substr(0, comma));
        std::wstring idxStr = util::Trim(spec.substr(comma + 1));
        iconIndex = static_cast<int>(std::wcstol(idxStr.c_str(), nullptr, 10));
    }

    // Strip quotes
    if (filePart.size() >= 2 && filePart.front() == L'"' && filePart.back() == L'"') {
        filePart = filePart.substr(1, filePart.size() - 2);
    }

    wchar_t expanded[MAX_PATH * 2]{};
    DWORD expLen = ExpandEnvironmentStringsW(filePart.c_str(), expanded, static_cast<DWORD>(sizeof(expanded) / sizeof(wchar_t)));
    if (expLen > 0 && expLen < sizeof(expanded) / sizeof(wchar_t)) {
        filePart = expanded;
    }

    if (filePart.empty()) return nullptr;

    HICON hLarge = nullptr;
    HICON hSmall = nullptr;
    UINT extracted = ExtractIconExW(filePart.c_str(), iconIndex, &hLarge, &hSmall, 1);
    if (extracted > 0) {
        if (hLarge) {
            if (hSmall) DestroyIcon(hSmall);
            return hLarge;
        }
        if (hSmall) return hSmall;
    }

    // Direct .ico file fallback
    HICON direct = static_cast<HICON>(LoadImageW(nullptr, filePart.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
    return direct;
}

std::unique_ptr<Gdiplus::Bitmap> AppIconCache::CreateBitmapFromHicon(HICON hIcon, int targetPx) {
    if (!hIcon) return nullptr;

    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    BITMAP bm{};
    HBITMAP refBmp = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    if (!refBmp || GetObjectW(refBmp, sizeof(bm), &bm) == 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }

    int srcW = bm.bmWidth;
    int srcH = ii.hbmColor ? bm.bmHeight : (bm.bmHeight / 2);
    if (srcW <= 0 || srcH <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }

    std::unique_ptr<Gdiplus::Bitmap> srcBmp;

    if (ii.hbmColor) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = srcW;
        bi.bmiHeader.biHeight = -srcH; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<DWORD> colorPixels(static_cast<size_t>(srcW) * static_cast<size_t>(srcH), 0);
        HDC hdc = GetDC(nullptr);
        bool gotColor = GetDIBits(hdc, ii.hbmColor, 0, static_cast<UINT>(srcH), colorPixels.data(), &bi, DIB_RGB_COLORS) != 0;

        bool hasAlpha = false;
        if (gotColor) {
            for (DWORD px : colorPixels) {
                if ((px & 0xFF000000u) != 0) {
                    hasAlpha = true;
                    break;
                }
            }
            if (!hasAlpha && ii.hbmMask) {
                std::vector<DWORD> maskPixels(static_cast<size_t>(srcW) * static_cast<size_t>(srcH), 0);
                if (GetDIBits(hdc, ii.hbmMask, 0, static_cast<UINT>(srcH), maskPixels.data(), &bi, DIB_RGB_COLORS) != 0) {
                    for (size_t i = 0; i < colorPixels.size(); ++i) {
                        bool transparent = (maskPixels[i] & 0x00FFFFFFu) != 0;
                        colorPixels[i] = transparent ? 0u : (colorPixels[i] | 0xFF000000u);
                    }
                }
            }
        }
        ReleaseDC(nullptr, hdc);

        if (gotColor) {
            srcBmp = std::make_unique<Gdiplus::Bitmap>(srcW, srcH, PixelFormat32bppARGB);
            Gdiplus::BitmapData bd{};
            Gdiplus::Rect lockRect(0, 0, srcW, srcH);
            if (srcBmp->LockBits(&lockRect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                for (int y = 0; y < srcH; ++y) {
                    BYTE* dstRow = static_cast<BYTE*>(bd.Scan0) + y * bd.Stride;
                    memcpy(dstRow, &colorPixels[static_cast<size_t>(y) * static_cast<size_t>(srcW)],
                           static_cast<size_t>(srcW) * sizeof(DWORD));
                }
                srcBmp->UnlockBits(&bd);
            }
        }
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (!srcBmp) {
        srcBmp.reset(Gdiplus::Bitmap::FromHICON(hIcon));
    }
    if (!srcBmp || srcBmp->GetLastStatus() != Gdiplus::Ok) return nullptr;

    if (srcW == targetPx && srcH == targetPx) {
        return srcBmp;
    }

    auto scaled = std::make_unique<Gdiplus::Bitmap>(targetPx, targetPx, PixelFormat32bppARGB);
    Gdiplus::Graphics g(scaled.get());
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.DrawImage(srcBmp.get(), 0, 0, targetPx, targetPx);
    return scaled;
}

std::unique_ptr<Gdiplus::Bitmap> AppIconCache::CreateFallbackBadge(const AppAudioInfo& app, int targetPx) {
    auto bmp = std::make_unique<Gdiplus::Bitmap>(targetPx, targetPx, PixelFormat32bppARGB);
    Gdiplus::Graphics g(bmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    Gdiplus::SolidBrush bg(Gdiplus::Color(235, 62, 72, 90));
    Gdiplus::Pen border(Gdiplus::Color(110, 255, 255, 255), 1.0f);
    float pad = 1.0f;
    float d = static_cast<float>(targetPx) - 2.0f * pad;
    g.FillEllipse(&bg, pad, pad, d, d);
    g.DrawEllipse(&border, pad, pad, d, d);

    wchar_t initial[2] = {L'A', L'\0'};
    if (!app.displayName.empty()) {
        initial[0] = app.displayName[0];
        CharUpperBuffW(initial, 1);
    }

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(targetPx) * 0.52f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush fg(Gdiplus::Color(255, 240, 244, 250));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF rc(0.0f, 0.0f, static_cast<float>(targetPx), static_cast<float>(targetPx));
    g.DrawString(initial, 1, &font, rc, &sf, &fg);
    return bmp;
}
