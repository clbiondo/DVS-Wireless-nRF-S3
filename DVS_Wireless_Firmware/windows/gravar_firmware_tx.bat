@echo off
setlocal enabledelayedexpansion
echo ============================================================
echo  Gravador de Firmware - TX nice!nano (DVS Wireless)
echo ============================================================
echo.
set TXDIR=%~dp0..\tx
set UF2FILE=
for %%F in ("%TXDIR%\*.uf2") do set UF2FILE=%%~nxF
if "%UF2FILE%"=="" (
    echo ERRO: nenhum .uf2 encontrado em %TXDIR%
    pause & exit /b 1
)
echo Firmware: %UF2FILE%
echo.
echo ANTES DE CONTINUAR:
echo   1. Conecte o TX via cabo USB
echo   2. Duplo-toque RAPIDO no botao de reset
echo   3. Espere NICENANO aparecer
pause >nul
echo.
echo Procurando NICENANO...
set NICEDRIVE=
set TENTATIVAS=0
:PROCURAR
set /a TENTATIVAS+=1
for /f "usebackq tokens=1" %%D in (`powershell -NoProfile -Command "(Get-Volume | Where-Object {$_.FileSystemLabel -eq 'NICENANO'}).DriveLetter" 2^>nul`) do set NICEDRIVE=%%D
if not "%NICEDRIVE%"=="" goto ENCONTRADO
if %TENTATIVAS% GEQ 15 (
    echo ERRO: NICENANO nao encontrado apos 30s.
    pause & exit /b 1
)
timeout /t 2 >nul
goto PROCURAR
:ENCONTRADO
echo Unidade: %NICEDRIVE%:
echo.
echo Copiando %UF2FILE%...
copy /y "%TXDIR%\%UF2FILE%" "%NICEDRIVE%:\%UF2FILE%" >nul
if errorlevel 1 (
    echo ERRO ao copiar.
) else (
    echo SUCESSO! TX gravado.
)
pause
