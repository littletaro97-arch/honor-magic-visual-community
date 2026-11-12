@echo off
setlocal
set "MagicDiagnosticPS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "MagicDiagnosticPS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%MagicDiagnosticPS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-ContextMenu.ps1" -Mode Response
set "MagicDiagnosticExit=%ERRORLEVEL%"
echo.
echo Diagnostic finished. Read the output path above before closing.
pause
exit /b %MagicDiagnosticExit%
