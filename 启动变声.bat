@echo off
chcp 936 >nul
mode con cols=100 lines=40 >nul 2>&1
title 实时变声器 - 正在运行
cd /d "%~dp0"
cls
echo ================================================================
echo                        实 时 变 声 器
echo ================================================================
echo.

if not exist "build\vc_live.exe" goto MISSING

rem ---- 1. 清理残留实例 ----
tasklist /fi "imagename eq vc_live.exe" 2>nul | find /i "vc_live.exe" >nul
if errorlevel 1 goto DEVICES
echo [提示] 检测到旧的 vc_live.exe 仍在运行，它会一直占用麦克风和播放设备。
echo        按任意键：结束旧实例并继续启动。
echo        想保留旧实例：请直接关闭本窗口。
echo.
pause >nul
taskkill /f /im vc_live.exe >nul 2>&1
echo [完成] 旧实例已结束。
echo.

:DEVICES
echo [1/2] 当前系统的音频设备如下：
echo.
build\vc_live.exe --list
echo.
echo    麦克风 0 = Realtek 麦克风阵列（本机唯一能正常拾音的设备）
echo    播放   0 = 当前默认播放设备（耳机）
echo.
echo [2/2] 5 秒后自动开始变声，默认参数：麦克风 0 / 播放 0 / 音调 1.5
echo        想改参数请按 Ctrl+C 中断，再运行 run_live.bat 自行指定。
echo.
timeout /t 5 /nobreak >nul 2>&1

echo.
echo ----------------------------------------------------------------
echo  变声已启动。本窗口请勿关闭，关闭窗口就等于停止变声。
echo  运行中可在本窗口直接输入并回车：
echo     数字      切换音调，例如 1.5 = 升调，0.8 = 更低沉
echo     s         查看当前状态和输入电平
echo     ?         查看帮助
echo     q         退出
echo ----------------------------------------------------------------
echo.

build\vc_live.exe --capture 0 --render 0 --pitch 1.5

echo.
echo ================================================================
echo  程序已退出。
echo  若上面出现 error / failed / 无法打开设备 等字样，把最后几行发给我。
echo ================================================================
pause
exit /b 0

:MISSING
echo [错误] 找不到 build\vc_live.exe
echo        请确认本文件与 build 文件夹在同一个目录下：
echo        %~dp0
echo.
pause
exit /b 1
