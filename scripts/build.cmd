@echo off
setlocal

if /I "%~1"=="-?" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%

:help
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Help
exit /b %ERRORLEVEL%
