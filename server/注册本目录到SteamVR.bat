@echo off
setlocal
cd /d "%~dp0"

set "VRPATH="
for /f "usebackq tokens=* delims=" %%P in ("%~dp0steamvr_path.txt") do if not defined VRPATH set "VRPATH=%%P"

if not defined VRPATH (
    echo [ERROR] steamvr_path.txt is empty.
    pause
    exit /b 1
)

set "VRPATHREG=%VRPATH%\bin\win64\vrpathreg.exe"
if not exist "%VRPATHREG%" (
    echo [ERROR] vrpathreg.exe not found: %VRPATHREG%
    echo         Fix the first line of steamvr_path.txt
    pause
    exit /b 1
)

echo Registering: %~dp0
"%VRPATHREG%" adddriver "%~dp0"
set "RC=%ERRORLEVEL%"
echo.
echo Exit code: %RC%
echo.
echo Reminder: only ONE ALVR driver directory may be registered.
echo If the old D:\HuaweiVRGlass_ALVR_Windows_v0.5.0 is still registered,
echo remove it first with:
echo   "%VRPATHREG%" removedriver "D:\HuaweiVRGlass_ALVR_Windows_v0.5.0"
echo.
pause
exit /b %RC%
