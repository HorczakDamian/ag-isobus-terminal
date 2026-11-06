@echo off
cd /d "%~dp0"
echo Starting ISOBUS Universal Terminal (VT Server) with AUX-N...
if exist "%~dp0build_vt\Release\isobus_vt_terminal.exe" (
    copy /Y "%~dp0build_vt\Release\isobus_vt_terminal.exe" "%~dp0isobus_vt_terminal.exe" >nul 2>&1
)
if exist "%~dp0isobus_vt_terminal.exe" (
    start "" "%~dp0isobus_vt_terminal.exe"
) else (
    start "" "%~dp0build_vt\Release\isobus_vt_terminal.exe"
)
