@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%ROOT%\MagicCommunity.exe' -ArgumentList '--installed-desktop','--composition-desktop' -WorkingDirectory '%ROOT%' -Verb RunAs"
