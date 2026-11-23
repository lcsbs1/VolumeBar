#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "model/AppIcons.h"
#include "ui/Menus.h"
#include "ui/Theme.h"

class MixerPanel {
public:
    MixerPanel(AudioEngine& engine, ConfigManager& config, AppIconCache& icons);
    ~MixerPanel();

    MixerPanel(const MixerPanel&) = delete;
    MixerPanel& operator=(const MixerPanel&) = delete;

    void SetCallbacks(const MenuCallbacks& cb) { callbacks_ = cb; }

    bool Create(HINSTANCE hInst);
    void Destroy();

    void Show(const RECT& anchorRect);
    void Hide();
    void Toggle(const RECT& anchorRect);
    bool IsVisible() const { return visible_; }

    void RefreshData();
    void UpdateTheme();

private:
    struct RowLayout {
        bool isMaster = false;
        std::wstring appId;
        RECT rowRect{};
        RECT muteRect{};
        RECT sliderRect{};
        RECT deviceRect{};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void ApplyDwmStyle();
    void RecomputeSizeAndPosition();
    void BuildLayout();
    void Render(HDC paintDc);
    void UpdatePeaks();

    void OnMouseDown(POINT pt);
    void OnMouseMove(POINT pt);
    void OnMouseUp();
    void OnMouseWheel(POINT pt, int delta, bool shift);

    float SliderRatioFromX(const RECT& sliderRect, int x) const;
    std::wstring DeviceShortName(const AppAudioInfo& app) const;

    AudioEngine& engine_;
    ConfigManager& config_;
    AppIconCache& icons_;
    MenuCallbacks callbacks_;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ui::Palette palette_;
    bool visible_ = false;
    bool suppressDeactivate_ = false;
    RECT lastAnchor_{};

    MasterAudioInfo master_;
    std::vector<AppAudioInfo> apps_;
    std::vector<AudioDeviceInfo> devices_;

    float masterPeak_ = 0.0f;
    std::unordered_map<std::wstring, float> appPeaks_;

    int scrollY_ = 0;
    int contentHeight_ = 0;
    int listTopY_ = 0;
    RECT profileBtnRect_{};
    std::vector<RowLayout> rows_;

    // Drag state
    bool dragging_ = false;
    bool dragIsMaster_ = false;
    std::wstring dragAppId_;
    RECT dragSliderRect_{};
};
