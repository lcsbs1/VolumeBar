#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "audio/AudioEngine.h"

class AppIconCache {
public:
    AppIconCache() = default;
    ~AppIconCache();

    AppIconCache(const AppIconCache&) = delete;
    AppIconCache& operator=(const AppIconCache&) = delete;

    Gdiplus::Bitmap* GetIcon(const AppAudioInfo& app, int targetPx);
    void Clear();

private:
    static std::unique_ptr<Gdiplus::Bitmap> CreateBitmapFromHicon(HICON hIcon, int targetPx);
    static HICON LoadIconFromPathSpec(const std::wstring& iconPath);
    static std::unique_ptr<Gdiplus::Bitmap> CreateFallbackBadge(const AppAudioInfo& app, int targetPx);

    std::unordered_map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> cache_;
};
