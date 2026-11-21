@echo off
setlocal
cd /d "%~dp0"

rem Resolve SteamVR from steamvr_path.txt (first line, plain text, no quotes)
set "VRPATH="
for /f "usebackq tokens=* delims=" %%P in ("%~dp0steamvr_path.txt") do if not defined VRPATH set "VRPATH=%%P"
if not defined VRPATH set "VRPATH=D:\Program Files\Steam\steamapps\common\SteamVR"
if not exist "%VRPATH%\bin\win64\vrmonitor.exe" set "VRPATH=D:\Program Files\Steam\steamapps\common\SteamVR"

echo SteamVR: %VRPATH%
if not exist "%VRPATH%\bin\win64\vrmonitor.exe" (
    echo [ERROR] vrmonitor.exe not found. Fix the first line of steamvr_path.txt
    pause
    exit /b 1
)

start "" "%~dp0ALVR Dashboard.exe"
timeout /t 8 /nobreak >nul
start "" "%VRPATH%\bin\win64\vrmonitor.exe"
exit /b 0
