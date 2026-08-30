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

echo Procurando portas COM...
set TEMPFILE=%TEMP%\dvs_ports_%RANDOM%.txt
powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object { if ($_.Name -match '(COM\d+)') { $matches[1] } }" > "%TEMPFILE%"

set COUNT=0
for /f "usebackq delims=" %%P in ("%TEMPFILE%") do (
    set /a COUNT+=1
    set "PORT_!COUNT!=%%P"
)
del "%TEMPFILE%" 2>nul

if !COUNT! EQU 0 (
    echo Nenhuma porta COM detectada automaticamente.
    set /p PORTA="Digite a porta COM manualmente (ex: COM5): "
) else if !COUNT! EQU 1 (
    set "PORTA=!PORT_1!"
    echo Porta detectada automaticamente: !PORTA!
) else (
    echo Multiplas portas encontradas:
    for /l %%i in (1,1,!COUNT!) do (
        echo   %%i^) !PORT_%%i!
    )
    set /p ESCOLHA="Digite o NUMERO DA LISTA acima (nao a porta em si): "
    for %%Z in (!ESCOLHA!) do set "PORTA=!PORT_%%Z!"
)

if "%PORTA%"=="" (
    echo ERRO: nenhuma porta valida selecionada.
    pause & exit /b 1
)

echo.
echo Gravando RX em %PORTA%...
esptool --chip esp32s3 -p %PORTA% -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB 0x0 "%RXDIR%\bootloader.bin" 0x10000 "%APP%" 0x8000 "%RXDIR%\partition-table.bin"
if errorlevel 1 (
    echo. & echo ERRO na gravacao.
) else (
    echo. & echo SUCESSO! RX gravado.
)
pause
