@echo off
setlocal EnableDelayedExpansion

set "CXX="
set "WINDRES="

where clang++ >nul 2>nul && set "CXX=clang++"
where llvm-windres >nul 2>nul && set "WINDRES=llvm-windres"

if not defined CXX (
    if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\clang++.exe" set "CXX=%LOCALAPPDATA%\Microsoft\WinGet\Links\clang++.exe"
)
if not defined WINDRES (
    if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\llvm-windres.exe" set "WINDRES=%LOCALAPPDATA%\Microsoft\WinGet\Links\llvm-windres.exe"
)

if not defined CXX (
    for /d %%P in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*") do (
        for /d %%D in ("%%P\llvm-mingw*") do (
            if exist "%%D\bin\clang++.exe" set "CXX=%%D\bin\clang++.exe"
            if exist "%%D\bin\llvm-windres.exe" set "WINDRES=%%D\bin\llvm-windres.exe"
        )
        if not defined CXX (
            if exist "%%P\bin\clang++.exe" set "CXX=%%P\bin\clang++.exe"
            if exist "%%P\bin\llvm-windres.exe" set "WINDRES=%%P\bin\llvm-windres.exe"
        )
    )
)

if not defined CXX (
    echo [VolumeBar] ERROR: clang++ not found. Install MartinStorsjo.LLVM-MinGW.UCRT via winget.
    exit /b 1
)
if not defined WINDRES (
    echo [VolumeBar] ERROR: llvm-windres not found.
    exit /b 1
)

if not exist build mkdir build

echo [VolumeBar] Compiling resources...
"%WINDRES%" resources.rc -O coff -o build\resources.res
if errorlevel 1 exit /b 1

set "SRCS="
for %%F in (src\*.cpp src\audio\*.cpp src\model\*.cpp src\config\*.cpp src\ui\*.cpp) do (
    if exist "%%F" set "SRCS=!SRCS! "%%F""
)

echo [VolumeBar] Building VolumeBar.exe...
"%CXX%" -std=c++17 -Os -s -Wall -Wextra -municode -mwindows ^
    -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 ^
    -Isrc ^
    -stdlib=libc++ -rtlib=compiler-rt -unwindlib=libunwind -static -static-libstdc++ ^
    -Wl,--gc-sections ^
    !SRCS! build\resources.res ^
    -o VolumeBar.exe ^
    -lole32 -loleaut32 -luuid -lgdi32 -lgdiplus -luser32 -lshell32 -lcomctl32 -ldwmapi -lshlwapi -ladvapi32 -lpropsys -lversion

if errorlevel 1 exit /b 1

for %%A in (VolumeBar.exe) do echo [VolumeBar] Built %%A (%%~zA bytes)
exit /b 0
