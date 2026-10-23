@echo off
title VAM Logger - Graph & Telemetry Viewer
cd /d "%~dp0"
cls
echo ========================================================
echo       VAM Multi-Channel Telemetry Graph Viewer
echo ========================================================
echo.
echo Launching Interactive Graph Viewer...
python log_viewer.py
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Trying fallback Python environment...
    C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe log_viewer.py
)
