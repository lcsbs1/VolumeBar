# VolumeBar

Mixer de volume por aplicativo ultraleve para Windows 11, posicionado no espaço vazio à esquerda da barra de tarefas. Escrito em **C++17 puro + Win32 + Core Audio (WASAPI)**, sem runtimes externos.

## Destaques de leveza

| Métrica | VolumeBar |
|---|---|
| **Executável** | ~370 KB, arquivo único, zero DLLs fora do Windows |
| **Memória privada ociosa** | ~2–3 MB |
| **CPU ocioso** | ~0% (100% orientado a eventos COM, sem polling com o painel fechado) |

## Recursos

- **Barrinha fixa na barra de tarefas:** ícone do volume mestre + ícones de cada aplicativo com sessão de áudio e uma barra fina de volume abaixo de cada ícone.
- **Apps pausados visíveis de forma discreta:** sessões pausadas/inativas permanecem na barra com opacidade reduzida para não deslocar os ícones.
- **Controle direto sem abrir janelas:**
  - **Roda do mouse** sobre um ícone ajusta o volume em `±2%` (`Shift + roda`: `±10%`) com tooltip em tempo real.
  - **Clique esquerdo** em um app muta/desmuta; no ícone **Mestre** abre/fecha o painel expandido.
  - **Clique direito** abre o menu de contexto com **Saída de áudio**, **Perfis**, **Abrir config.json**, **Iniciar com o Windows** e **Sair**.
- **Painel expandido (Windows 11 Acrylic):** popup acima da barrinha com sliders para o volume mestre e cada app, medidores de pico em tempo real (30 fps apenas enquanto aberto) e seletor de dispositivo de saída por app.
- **Saída de áudio por app (experimental):** roteamento persistente por processo via `Windows.Media.Internal.AudioPolicyConfig` (Windows 10 21H2+ / Windows 11), isolado em [AudioPolicyConfig.cpp](src/audio/AudioPolicyConfig.cpp) com fallback seguro caso uma atualização do Windows altere a interface interna.
- **Perfis e atalhos globais:** aplique combinações de volume via menu ou atalhos globais (`Ctrl+Alt+V` abre o painel, `Ctrl+Alt+1` / `Ctrl+Alt+2` aplicam perfis) e salve o estado atual como novo perfil.
- **Integração com o Windows:** instância única (mutex), ocultação automática em jogos/vídeos em tela cheia, recuperação automática ao reiniciar o `explorer.exe` (`TaskbarCreated`), Per-Monitor DPI Aware v2 e suporte a tema escuro/claro.

## Uso via linha de comando (CLI)

```powershell
.\VolumeBar.exe --list
.\VolumeBar.exe --set master 50
.\VolumeBar.exe --set spotify.exe 30
.\VolumeBar.exe --toggle-mute discord.exe
.\VolumeBar.exe --set-device spotify.exe default
```

## Compilação

Requer **LLVM-MinGW (UCRT)** no Windows 10/11:

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --accept-source-agreements --accept-package-agreements
.\build.bat
```

O script `build.bat` localiza o `clang++` e o `llvm-windres` automaticamente e gera `VolumeBar.exe` na raiz do projeto.

## Configuração

Na primeira execução, o arquivo `%APPDATA%\VolumeBar\config.json` é criado automaticamente:

```json
{
  "maxWidth": 360,
  "scrollStep": 2,
  "hotkeys": { "openPanel": "Ctrl+Alt+V" },
  "profiles": {
    "Jogo": { "hotkey": "Ctrl+Alt+1", "apps": { "spotify.exe": 20, "discord.exe": 80 } },
    "Música": { "hotkey": "Ctrl+Alt+2", "apps": { "spotify.exe": 100, "discord.exe": 40 } }
  }
}
```

## Estrutura do projeto

```text
├── build.bat                 # Script de build (LLVM-MinGW UCRT)
├── app.manifest              # Per-Monitor DPI v2 e Common Controls v6
├── resources.rc              # Ícone e manifest embutidos
├── assets/volumebar.ico      # Ícone multi-resolução
└── src/
    ├── main.cpp              # CLI, instância única, hotkeys e loop Win32
    ├── audio/
    │   ├── AudioEngine.*     # Core Audio (WASAPI), sessões e eventos COM
    │   └── AudioPolicyConfig.* # Troca de saída por app (experimental)
    ├── config/Config.*       # JSON de configuração, perfis, atalhos e autostart
    ├── model/AppIcons.*      # Extração e cache de ícones por processo
    ├── ui/
    │   ├── TaskbarWidget.*   # Barrinha layered sobre a barra de tarefas + tooltip
    │   ├── MixerPanel.*      # Painel expandido Acrylic + medidores de pico
    │   ├── Menus.*           # Menus de contexto e diálogo de perfil
    │   └── Theme.h           # Paleta claro/escuro, DPI e glifos GDI+
    └── util/                 # ComPtr, Strings e Messages
```

## Observações

- Recomenda-se deixar o botão **Widgets** da barra de tarefas do Windows 11 desativado para manter o espaço esquerdo livre.
- Se o **Controle Inteligente de Aplicativos (Smart App Control)** do Windows 11 estiver ativo em modo de imposição, binários compilados localmente sem assinatura digital podem ser bloqueados pelo sistema.
