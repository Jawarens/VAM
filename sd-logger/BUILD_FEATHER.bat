@echo off
setlocal
echo ===================================================
echo   VAM Feather ESP32-S3 Build Utility (ESP-IDF v6.1)
echo ===================================================

echo Activating ESP-IDF environment...
call C:\esp\v6.1-beta1\esp-idf\export.bat >nul 2>&1

echo Building project...
idf.py build

if %ERRORLEVEL% equ 0 (
    echo.
    echo [SUCCESS] Build succeeded! Run FLASH_FEATHER.bat to upload.
) else (
    echo.
    echo [ERROR] Build failed.
)

pause

