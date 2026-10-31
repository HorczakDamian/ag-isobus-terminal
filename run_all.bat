@echo off
title isoVT - Launcher Zestawu
color 0A
cls
echo ====================================================================
echo                   isoVT Precision Ag Ecosystem
echo ====================================================================
echo.
echo Wybierz opcje uruchomienia:
echo [1] Uruchom tylko Terminal isoVT (ISO 11783-6)
echo [2] Uruchom pelny zestaw (AgIO + AgOpenGPS + isoVT)
echo [3] Sprawdz instrukcje instalacji sterownikow PCAN-USB
echo [4] Wyjscie
echo.
set /p choice="Wpisz numer (1-4) i wcisnij Enter: "

if "%choice%"=="1" goto run_terminal
if "%choice%"=="2" goto run_full
if "%choice%"=="3" goto show_drivers
if "%choice%"=="4" goto end

:run_terminal
echo Uruchamianie terminala isoVT...
start "" "%~dp0isobus_vt_terminal.exe"
exit /b

:run_full
echo Uruchamianie AgIO...
if exist "%~dp0agopen\AgIO.exe" start "" "%~dp0agopen\AgIO.exe"
timeout /t 2 /nobreak >nul

echo Uruchamianie AgOpenGPS...
if exist "%~dp0agopen\AgOpenGPS.exe" start "" "%~dp0agopen\AgOpenGPS.exe"
timeout /t 3 /nobreak >nul

echo Uruchamianie terminala isoVT...
start "" "%~dp0isobus_vt_terminal.exe"
exit /b

:show_drivers
if exist "%~dp0DRIVERS_PEAK_PCAN.txt" notepad "%~dp0DRIVERS_PEAK_PCAN.txt"
exit /b

:end
exit /b
