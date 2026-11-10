@echo off
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "DOTNET_ROOT=%ROOT%\dotnet"
set "DOTNET_ROOT_X64=%ROOT%\dotnet"
set "DOTNET_MULTILEVEL_LOOKUP=0"
set "DOTNET_CLI_TELEMETRY_OPTOUT=1"
set "DOTNET_NOLOGO=1"
start "" "%ROOT%\MagicCommunity.exe" --root "%ROOT%" --composition-desktop
