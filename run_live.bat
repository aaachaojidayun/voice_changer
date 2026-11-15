@echo off
rem Live pitch-shift test launcher.
rem Usage: run_live.bat [capture_index] [render_index] [pitch]
rem   run_live.bat             -> microphone 0, headphones 0, pitch 1.5
rem   run_live.bat 0 0 0.8     -> same devices, pitch 0.8
rem   run_live.bat --list      -> just list device indexes

chcp 65001 >nul
cd /d "%~dp0"

rem 双击运行时不会继承开发环境的 PATH，这里补上 MinGW（只在需要重新编译时用到）
if exist "D:\mingw64\bin\g++.exe" set "PATH=D:\mingw64\bin;%PATH%"

if not exist build\vc_live.exe (
  echo [build] compiling vc_live.exe ...
  if not exist build mkdir build
  g++ -std=c++17 -O2 -Iinclude src\fft.cpp src\resampler.cpp src\phase_vocoder.cpp ^
      src\pitch_shifter.cpp src\wav_io.cpp src\live_win.cpp -o build\vc_live.exe -lole32 -luuid
  if errorlevel 1 (
    echo [build] failed.
    pause
    exit /b 1
  )
)

if /i "%~1"=="--list" (
  build\vc_live.exe --list
  pause
  exit /b 0
)

set CAP=%~1
set REN=%~2
set PIT=%~3
if "%PIT%"=="" set PIT=1.5

rem 默认用 0 号设备：本机实测只有"麦克风阵列 (Realtek(R) Audio)"能真正拾音
rem （MCHOSE V9 PRO 无线耳机的麦克风在系统里几乎无信号，约 -110 dBFS，等于静音）。
rem 换了设备先跑: run_live.bat --list 看编号。
if "%CAP%"=="" set CAP=0
if "%REN%"=="" set REN=0

build\vc_live.exe --capture %CAP% --render %REN% --pitch %PIT%
pause
