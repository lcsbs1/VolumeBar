#include "ui/Menus.h"

#include <shellapi.h>
#include <vector>

#include "util/Strings.h"

namespace ui {

namespace {

constexpr UINT kCmdOpenPanel      = 1001;
constexpr UINT kCmdSaveProfile    = 1002;
constexpr UINT kCmdOpenConfig     = 1003;
constexpr UINT kCmdReloadConfig   = 1004;
constexpr UINT kCmdAutostart      = 1005;
constexpr UINT kCmdExit           = 1006;
constexpr UINT kCmdToggleMute     = 1010;
constexpr UINT kCmdDeviceDefault  = 1100;
constexpr UINT kCmdDeviceBase     = 1101;
constexpr UINT kCmdProfileBase    = 2000;

HMENU BuildProfilesSubmenu(const ConfigManager& config, std::vector<std::wstring>* outNames) {
    HMENU sub = CreatePopupMenu();
    UINT idx = 0;
    for (const auto& kv : config.Data().profiles) {
        std::wstring label = kv.first;
        if (!kv.second.hotkey.empty()) {
            label += L"\t" + kv.second.hotkey;
        }
        AppendMenuW(sub, MF_STRING, kCmdProfileBase + idx, label.c_str());
        outNames->push_back(kv.first);
        ++idx;
    }
    if (idx > 0) {
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(sub, MF_STRING, kCmdSaveProfile, L"Salvar perfil atual...");
    return sub;
}

HMENU BuildDeviceSubmenu(const AppAudioInfo& app, const AudioEngine& engine,
                         std::vector<AudioDeviceInfo>* outDevices) {
    HMENU sub = CreatePopupMenu();
    bool available = engine.IsPerAppRoutingAvailable() && !app.isSystemSounds;
    *outDevices = engine.GetDevices();

    if (!available) {
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0,
                    app.isSystemSounds ? L"Indisponível para Sons do sistema"
                                       : L"Indisponível nesta versão do Windows");
        return sub;
    }

    UINT defFlags = MF_STRING | (app.persistedDeviceId.empty() ? MF_CHECKED : 0);
    AppendMenuW(sub, defFlags, kCmdDeviceDefault, L"Padrão do Windows");
    if (!outDevices->empty()) {
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    }

    for (size_t i = 0; i < outDevices->size(); ++i) {
        const auto& d = (*outDevices)[i];
        bool checked = (!app.persistedDeviceId.empty() && app.persistedDeviceId == d.id);
        UINT flags = MF_STRING | (checked ? MF_CHECKED : 0);
        std::wstring label = d.name;
        if (d.isDefault) label += L" (Padrão)";
        AppendMenuW(sub, flags, kCmdDeviceBase + static_cast<UINT>(i), label.c_str());
    }
    return sub;
}

// Lightweight modal input window for naming a new profile.
struct PromptState {
    std::wstring result;
    bool accepted = false;
    bool done = false;
    HWND editHwnd = nullptr;
    HFONT font = nullptr;
};

constexpr wchar_t kPromptClass[] = L"VolumeBar_ProfilePrompt";
constexpr int kPromptEditId = 101;
constexpr int kPromptOkId = 102;
constexpr int kPromptCancelId = 103;

LRESULT CALLBACK PromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            if (!st) return -1;
            UINT dpi = GetDpiForWindow(hwnd);
            auto sc = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
            st->font = CreateFontW(-sc(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

            HWND lbl = CreateWindowExW(0, L"STATIC", L"Nome do perfil:",
                                       WS_CHILD | WS_VISIBLE,
                                       sc(14), sc(12), sc(260), sc(20),
                                       hwnd, nullptr, nullptr, nullptr);
            st->editHwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           sc(14), sc(36), sc(260), sc(26),
                                           hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptEditId)),
                                           nullptr, nullptr);
            HWND okBtn = CreateWindowExW(0, L"BUTTON", L"Salvar",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                         sc(110), sc(74), sc(78), sc(28),
                                         hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptOkId)),
                                         nullptr, nullptr);
            HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancelar",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                             sc(196), sc(74), sc(78), sc(28),
                                             hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptCancelId)),
                                             nullptr, nullptr);
            if (st->font) {
                SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                SendMessageW(st->editHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                SendMessageW(okBtn, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
                SendMessageW(cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            }
            SetFocus(st->editHwnd);
            return 0;
        }
        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);
            if (id == kPromptOkId || id == IDOK) {
                wchar_t buf[128]{};
                GetWindowTextW(st->editHwnd, buf, static_cast<int>(sizeof(buf) / sizeof(wchar_t)));
                st->result = util::Trim(buf);
                st->accepted = !st->result.empty();
                st->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kPromptCancelId || id == IDCANCEL) {
                st->accepted = false;
                st->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (st) {
                st->accepted = false;
                st->done = true;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st && st->font) {
                DeleteObject(st->font);
                st->font = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HandleCommonCommand(UINT cmd, HWND owner, AudioEngine& engine, ConfigManager& config,
                         const std::vector<std::wstring>& profileNames, const MenuCallbacks& cb) {
    if (cmd == kCmdOpenPanel) {
        if (cb.onOpenPanel) cb.onOpenPanel();
    } else if (cmd == kCmdSaveProfile) {
        std::wstring name;
        if (PromptProfileName(owner, &name)) {
            config.SaveCurrentAsProfile(name, engine);
            if (cb.onReloadConfig) cb.onReloadConfig();
        }
    } else if (cmd == kCmdOpenConfig) {
        std::wstring cfgPath = config.GetConfigPath();
        ShellExecuteW(nullptr, L"open", cfgPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == kCmdReloadConfig) {
        config.Load();
        if (cb.onReloadConfig) cb.onReloadConfig();
    } else if (cmd == kCmdAutostart) {
        ConfigManager::SetAutostart(!ConfigManager::IsAutostartEnabled());
    } else if (cmd == kCmdExit) {
        if (cb.onExit) cb.onExit();
    } else if (cmd >= kCmdProfileBase && cmd < kCmdProfileBase + profileNames.size()) {
        const std::wstring& profName = profileNames[cmd - kCmdProfileBase];
        config.ApplyProfile(profName, engine);
        if (cb.onStateChanged) cb.onStateChanged();
    }
}

} // namespace

bool PromptProfileName(HWND owner, std::wstring* outName) {
    if (!outName) return false;

    static bool registered = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PromptWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kPromptClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    PromptState state;
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int w = 304;
    int h = 152;
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2;
    int y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        kPromptClass, L"VolumeBar — Salvar Perfil",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        owner, nullptr, hInst, &state);
    if (!dlg) return false;

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(dlg, WM_COMMAND, kPromptOkId, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(dlg, WM_COMMAND, kPromptCancelId, 0);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (state.accepted) {
        *outName = state.result;
        return true;
    }
    return false;
}

void ShowGeneralMenu(HWND owner, POINT screenPt, AudioEngine& engine, ConfigManager& config,
                     const MenuCallbacks& cb) {
    HMENU menu = CreatePopupMenu();
    std::vector<std::wstring> profileNames;
    HMENU profSub = BuildProfilesSubmenu(config, &profileNames);

    AppendMenuW(menu, MF_STRING, kCmdOpenPanel, L"Abrir painel");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profSub), L"Perfis");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenConfig, L"Abrir config.json");
    AppendMenuW(menu, MF_STRING, kCmdReloadConfig, L"Recarregar configuração");
    AppendMenuW(menu, MF_STRING | (ConfigManager::IsAutostartEnabled() ? MF_CHECKED : 0),
                kCmdAutostart, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Sair");

    SetForegroundWindow(owner);
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        screenPt.x, screenPt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd != 0) {
        HandleCommonCommand(cmd, owner, engine, config, profileNames, cb);
    }
}

void ShowAppMenu(HWND owner, POINT screenPt, const AppAudioInfo& app, AudioEngine& engine,
                 ConfigManager& config, const MenuCallbacks& cb) {
    HMENU menu = CreatePopupMenu();
    std::vector<std::wstring> profileNames;
    std::vector<AudioDeviceInfo> devices;
    HMENU devSub = BuildDeviceSubmenu(app, engine, &devices);
    HMENU profSub = BuildProfilesSubmenu(config, &profileNames);

    std::wstring header = app.displayName;
    if (!app.isActive) header += L" (Pausado)";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggleMute, app.muted ? L"Desmutar" : L"Mutar");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(devSub), L"Saída de áudio");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profSub), L"Perfis");
    AppendMenuW(menu, MF_STRING, kCmdOpenPanel, L"Abrir painel");
    AppendMenuW(menu, MF_STRING | (ConfigManager::IsAutostartEnabled() ? MF_CHECKED : 0),
                kCmdAutostart, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Sair");

    SetForegroundWindow(owner);
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        screenPt.x, screenPt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == 0) return;

    if (cmd == kCmdToggleMute) {
        engine.ToggleAppMute(app.appId);
        if (cb.onStateChanged) cb.onStateChanged();
    } else if (cmd == kCmdDeviceDefault) {
        engine.SetAppOutputDevice(app.appId, L"");
        if (cb.onStateChanged) cb.onStateChanged();
    } else if (cmd >= kCmdDeviceBase && cmd < kCmdDeviceBase + devices.size()) {
        engine.SetAppOutputDevice(app.appId, devices[cmd - kCmdDeviceBase].id);
        if (cb.onStateChanged) cb.onStateChanged();
    } else {
        HandleCommonCommand(cmd, owner, engine, config, profileNames, cb);
    }
}

void ShowDevicePickerMenu(HWND owner, POINT screenPt, const AppAudioInfo& app, AudioEngine& engine,
                          const std::function<void()>& onChanged) {
    std::vector<AudioDeviceInfo> devices;
    HMENU menu = BuildDeviceSubmenu(app, engine, &devices);

    SetForegroundWindow(owner);
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        screenPt.x, screenPt.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == kCmdDeviceDefault) {
        engine.SetAppOutputDevice(app.appId, L"");
        if (onChanged) onChanged();
    } else if (cmd >= kCmdDeviceBase && cmd < kCmdDeviceBase + devices.size()) {
        engine.SetAppOutputDevice(app.appId, devices[cmd - kCmdDeviceBase].id);
        if (onChanged) onChanged();
    }
}

} // namespace ui
