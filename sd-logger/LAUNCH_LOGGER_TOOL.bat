@echo off
title VAM Logger - USB-C Manager
cd /d "%~dp0"
cls
echo ========================================================
echo       VAM Multi-Channel Telemetry Logger Manager
echo ========================================================
echo.
echo Starting USB-C Manager...
python download_logs.py
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Trying fallback Python environment...
    C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe download_logs.py
)
echo.
echo ========================================================
echo Session finished.
echo ========================================================
pause
