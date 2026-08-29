@echo off
setlocal enabledelayedexpansion
echo ============================================================
echo  Gravador de Firmware - RX ESP32-S3 (DVS Wireless)
echo ============================================================
echo.
set RXDIR=%~dp0..\rx
python --version >nul 2>nul
if errorlevel 1 (
    echo ERRO: Python nao instalado. Instale em https://python.org
    pause & exit /b 1
)
where esptool >nul 2>nul
if errorlevel 1 (
    echo Instalando esptool...
    python -m pip install esptool
)
echo OK: ferramentas prontas.
echo.
if not exist "%RXDIR%\bootloader.bin" (
    echo ERRO: bootloader.bin nao encontrado em %RXDIR%
    pause & exit /b 1
)
if not exist "%RXDIR%\partition-table.bin" (
    echo ERRO: partition-table.bin nao encontrado em %RXDIR%
    pause & exit /b 1
)
set APP=
for %%F in ("%RXDIR%\*.bin") do (
    if not "%%~nF"=="bootloader" if not "%%~nF"=="partition-table" set APP=%%F
)
if "%APP%"=="" (
    echo ERRO: firmware .bin nao encontrado em %RXDIR%
    pause & exit /b 1
)
echo Firmware: %APP%
echo.
set /p PORTA="Digite a porta COM (ex: COM5): "
if "%PORTA%"=="" echo ERRO: nenhuma porta. & pause & exit /b 1
echo.
echo Gravando RX em %PORTA%...
esptool --chip esp32s3 -p %PORTA% -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB 0x0 "%RXDIR%\bootloader.bin" 0x10000 "%APP%" 0x8000 "%RXDIR%\partition-table.bin"
if errorlevel 1 (
    echo. & echo ERRO na gravacao.
) else (
    echo. & echo SUCESSO! RX gravado.
)
pause
