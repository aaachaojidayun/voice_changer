@echo off
rem Live pitch-shift test launcher.
rem Usage: run_live.bat [capture_index] [render_index] [pitch]
rem   run_live.bat             -> microphone 0, headphones 0, pitch 1.5
rem   run_live.bat 0 0 0.8     -> same devices, pitch 0.8
rem   run_live.bat --list      -> just list device indexes

chcp 65001 >nul
cd /d "%~dp0"

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
if "%CAP%"=="" set CAP=0
if "%REN%"=="" set REN=0
if "%PIT%"=="" set PIT=1.5

build\vc_live.exe --capture %CAP% --render %REN% --pitch %PIT%
pause
