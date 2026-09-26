@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1"
set "code=%errorlevel%"
if not "%code%"=="0" pause
exit /b %code%
