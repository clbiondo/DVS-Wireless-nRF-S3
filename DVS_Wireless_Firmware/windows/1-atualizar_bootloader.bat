@echo off
setlocal enabledelayedexpansion
echo ============================================================
echo  Atualizador de Bootloader - nice!nano (DVS Wireless)
echo ============================================================
echo.
set TXDIR=%~dp0..\tx
set PACOTE=%~dp0nice_nano_bootloader-0.10.0_s140_6.1.1.zip

python --version >nul 2>nul
if errorlevel 1 (
    echo ERRO: Python nao instalado. Instale em https://python.org
    pause & exit /b 1
)
echo OK: Python encontrado.
echo.

where adafruit-nrfutil >nul 2>nul
if errorlevel 1 (
    echo AVISO: adafruit-nrfutil nao encontrado. Instalando...
    python -m pip install adafruit-nrfutil
    where adafruit-nrfutil >nul 2>nul
    if errorlevel 1 (
        echo ERRO: nao foi possivel instalar adafruit-nrfutil.
        echo Tente manualmente: python -m pip install adafruit-nrfutil
        pause & exit /b 1
    )
)
echo OK: ferramentas encontradas.
echo.

if not exist "%PACOTE%" (
    echo ERRO: nao encontrei o arquivo "%PACOTE%"
    pause & exit /b 1
)
echo Pacote encontrado: %PACOTE%
echo.

echo ANTES DE CONTINUAR:
echo   1. Conecte o TX via cabo USB direto no computador
echo   2. De um duplo-toque RAPIDO no botao de reset
echo   3. Confirme que apareceu "NICENANO" no Explorador de Arquivos
pause >nul
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
echo ============================================================
echo  ATENCAO: atualizar o BOOTLOADER e sensivel.
echo  Nao desconecte o cabo USB nem desligue o computador.
echo ============================================================
echo.
set /p CONFIRMA="Tem certeza que quer continuar? (S/N): "
if /i not "%CONFIRMA%"=="S" (
    echo Operacao cancelada.
    pause & exit /b 0
)

echo.
echo Gravando bootloader em %PORTA%...
echo.
adafruit-nrfutil dfu serial --package "%PACOTE%" -p %PORTA% -b 115200 --singlebank
if errorlevel 1 (
    echo. & echo ERRO durante a gravacao. Verifique a porta e tente novamente.
) else (
    echo. & echo SUCESSO! Bootloader atualizado. A placa deve reiniciar sozinha.
)
pause
