#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "model/AppIcons.h"
#include "ui/MixerPanel.h"
#include "ui/TaskbarWidget.h"
#include "util/Messages.h"
#include "util/Strings.h"

namespace {

constexpr wchar_t kSingleInstanceMutex[] = L"Local\\VolumeBar_SingleInstance_Mutex";

int ToPercent(float v) {
    int p = static_cast<int>(std::lround(v * 100.0f));
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

bool ParsePercent(const wchar_t* arg, float* outLevel) {
    if (!arg || !*arg || !outLevel) return false;
    wchar_t* endPtr = nullptr;
    long val = std::wcstol(arg, &endPtr, 10);
    if (endPtr == arg || *endPtr != L'\0' || val < 0 || val > 100) {
        return false;
    }
    *outLevel = static_cast<float>(val) / 100.0f;
    return true;
}

void PrintUsage() {
    util::WriteOut(
        L"VolumeBar CLI\r\n"
        L"  VolumeBar.exe --list\r\n"
        L"  VolumeBar.exe --set <master|appId> <0-100>\r\n"
        L"  VolumeBar.exe --mute <master|appId>\r\n"
        L"  VolumeBar.exe --unmute <master|appId>\r\n"
        L"  VolumeBar.exe --toggle-mute <master|appId>\r\n"
        L"  VolumeBar.exe --set-device <appId> <deviceId|default>\r\n");
}

int RunCli(int argc, wchar_t* argv[]) {
    util::InitCliConsole();

    std::wstring cmd = argv[1];
    if (cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
        PrintUsage();
        return 0;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        util::WriteErr(L"Erro ao inicializar COM.\r\n");
        return 1;
    }

    AudioEngine engine;
    if (!engine.Initialize(nullptr)) {
        util::WriteErr(L"Erro ao inicializar AudioEngine.\r\n");
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return 1;
    }

    int exitCode = 0;

    if (cmd == L"--list") {
        MasterAudioInfo master = engine.GetMasterInfo();
        auto devices = engine.GetDevices();
        auto apps = engine.GetApps();

        wchar_t line[1024]{};
        wsprintfW(line, L"Master: %d%% [%s] - %s\r\n",
                  ToPercent(master.volume),
                  master.muted ? L"MUTED" : L"OK",
                  master.defaultDeviceName.empty() ? L"(nenhum dispositivo)" : master.defaultDeviceName.c_str());
        util::WriteOut(line);

        util::WriteOut(L"AudioPolicyConfig: ");
        util::WriteOut(engine.PerAppRoutingStatus());
        util::WriteOut(L"\r\n\r\nDispositivos de saida:\r\n");

        for (const auto& d : devices) {
            std::wstring devLine = L"  ";
            devLine += d.isDefault ? L"* " : L"  ";
            devLine += d.name + L"  [" + d.id + L"]\r\n";
            util::WriteOut(devLine);
        }

        util::WriteOut(L"\r\nSessoes de audio:\r\n");
        if (apps.empty()) {
            util::WriteOut(L"  (nenhuma sessao ativa/inativa)\r\n");
        } else {
            for (const auto& a : apps) {
                std::wstring pidsStr;
                for (size_t i = 0; i < a.pids.size(); ++i) {
                    if (i > 0) pidsStr += L",";
                    pidsStr += std::to_wstring(a.pids[i]);
                }
                if (pidsStr.empty()) pidsStr = L"0";
                std::wstring appLine = L"  " + a.appId + L" (" + a.displayName + L")  "
                    + std::to_wstring(ToPercent(a.volume)) + L"%"
                    + (a.muted ? L" [MUTED]" : L"")
                    + (a.isActive ? L" [Ativo]" : L" [Pausado]")
                    + L"  PID=" + pidsStr + L"\r\n";
                util::WriteOut(appLine);
            }
        }
    } else if (cmd == L"--set" && argc >= 4) {
        std::wstring target = argv[2];
        float level = 0.0f;
        if (!ParsePercent(argv[3], &level)) {
            util::WriteErr(L"Volume invalido (use 0..100).\r\n");
            exitCode = 1;
        } else if (util::EqualsIgnoreCase(target, L"master")) {
            if (!engine.SetMasterVolume(level)) {
                util::WriteErr(L"Falha ao ajustar volume mestre.\r\n");
                exitCode = 1;
            }
        } else {
            std::wstring resolved;
            if (!engine.FindAppId(target, &resolved) || !engine.SetAppVolume(resolved, level)) {
                util::WriteErr(L"App nao encontrado ou falha ao ajustar volume.\r\n");
                exitCode = 1;
            }
        }
    } else if ((cmd == L"--mute" || cmd == L"--unmute" || cmd == L"--toggle-mute") && argc >= 3) {
        std::wstring target = argv[2];
        if (util::EqualsIgnoreCase(target, L"master")) {
            bool ok = (cmd == L"--mute")   ? engine.SetMasterMute(true) :
                      (cmd == L"--unmute") ? engine.SetMasterMute(false) :
                                             engine.ToggleMasterMute();
            if (!ok) {
                util::WriteErr(L"Falha ao alterar mute mestre.\r\n");
                exitCode = 1;
            }
        } else {
            std::wstring resolved;
            bool ok = engine.FindAppId(target, &resolved);
            if (ok) {
                ok = (cmd == L"--mute")   ? engine.SetAppMute(resolved, true) :
                     (cmd == L"--unmute") ? engine.SetAppMute(resolved, false) :
                                            engine.ToggleAppMute(resolved);
            }
            if (!ok) {
                util::WriteErr(L"App nao encontrado ou falha ao alterar mute.\r\n");
                exitCode = 1;
            }
        }
    } else if (cmd == L"--set-device" && argc >= 4) {
        std::wstring resolved;
        std::wstring devArg = argv[3];
        if (util::EqualsIgnoreCase(devArg, L"default")) devArg.clear();
        if (!engine.FindAppId(argv[2], &resolved) || !engine.SetAppOutputDevice(resolved, devArg)) {
            util::WriteErr(L"Falha ao alterar dispositivo do app.\r\n");
            exitCode = 1;
        }
    } else {
        PrintUsage();
        exitCode = 1;
    }

    engine.Shutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return exitCode;
}

int RunGui(HINSTANCE hInst) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(TaskbarWidget::kWindowClass, nullptr);
        if (existing) {
            PostMessageW(existing, msgs::WM_APP_TOGGLE_PANEL, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        CloseHandle(mutex);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) != Gdiplus::Ok) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    {
        ConfigManager config;
        config.Load();

        AudioEngine engine;
        AppIconCache icons;
        MixerPanel panel(engine, config, icons);
        TaskbarWidget widget(engine, config, icons, panel);

        if (panel.Create(hInst) && widget.Create(hInst)) {
            engine.Initialize(widget.GetHwnd());
            widget.RefreshData();
            panel.RefreshData();

            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            engine.Shutdown();
            panel.Destroy();
            widget.Destroy();
        }
        icons.Clear();
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    CloseHandle(mutex);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        int code = RunCli(argc, argv);
        LocalFree(argv);
        return code;
    }
    if (argv) LocalFree(argv);

    return RunGui(hInst);
}
