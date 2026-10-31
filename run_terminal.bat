@echo off
cd /d "%~dp0"
echo Starting ISOBUS Universal Terminal (VT Server) with AUX-N...
start "" "build_vt\Release\isobus_vt_terminal.exe"
