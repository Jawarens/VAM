@echo off
setlocal
echo ===================================================
echo   VAM Feather ESP32-S3 Flash Utility (ESP-IDF v6.1)
echo ===================================================

if "%~1"=="" (
    set /p PORT="Enter COM port (e.g. COM4): "
) else (
    set PORT=%~1
)

echo.
echo Activating ESP-IDF environment...
call C:\esp\v6.1-beta1\esp-idf\export.bat >nul 2>&1

echo Flashing firmware to %PORT%...
python -m esptool --chip esp32s3 -p %PORT% -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 8MB --flash-freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\sd-logger.bin

if %ERRORLEVEL% equ 0 (
    echo.
    echo [SUCCESS] Flash completed successfully!
) else (
    echo.
    echo [ERROR] Flashing failed. Check COM port and connection.
)

pause

