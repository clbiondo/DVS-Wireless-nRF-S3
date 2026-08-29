#!/bin/bash
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
RX_DIR="$DIR/../rx"

echo ""
echo " Gravador de Firmware - RX ESP32-S3 (DVS Wireless)"
echo ""
echo ""

OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Darwin) OS_NAME="macOS" ;;
    Linux)  OS_NAME="Linux"  ;;
    *)      OS_NAME="$OS_TYPE" ;;
esac
echo "Sistema: $OS_NAME"

if command -v python3 &>/dev/null; then
    PYTHON="python3"
elif command -v python &>/dev/null; then
    PYTHON="python"
else
    echo "ERRO: Python nao encontrado."
    echo "  macOS:  brew install python3"
    echo "  Linux:  sudo apt install python3"
    exit 1
fi
echo "Python: $($PYTHON --version 2>&1)"

if ! $PYTHON -m esptool --version &>/dev/null 2>&1; then
    echo "Instalando esptool..."
    $PYTHON -m pip install esptool
fi
echo "esptool: OK"
echo ""

BOOTLOADER="$RX_DIR/bootloader.bin"
PARTTABLE="$RX_DIR/partition-table.bin"
APP=$(find "$RX_DIR" -name "nrf24_test.bin" -o -name "*.bin" ! -name "bootloader.bin" ! -name "partition-table.bin" | head -1)

if [ ! -f "$BOOTLOADER" ]; then
    echo "ERRO: bootloader.bin nao encontrado em $RX_DIR/"
    exit 1
fi
if [ ! -f "$PARTTABLE" ]; then
    echo "ERRO: partition-table.bin nao encontrado em $RX_DIR/"
    exit 1
fi
if [ -z "$APP" ] || [ ! -f "$APP" ]; then
    echo "ERRO: firmware .bin nao encontrado em $RX_DIR/"
    exit 1
fi
echo "Arquivos: $(basename $BOOTLOADER), $(basename $PARTTABLE), $(basename $APP)"
echo ""

PORTA=""
if [ "$OS_TYPE" = "Darwin" ]; then
    PORTA="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
    [ -z "$PORTA" ] && PORTA="$(ls /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1)"
    [ -z "$PORTA" ] && PORTA="$(ls /dev/cu.usbserial* 2>/dev/null | head -1)"
elif [ "$OS_TYPE" = "Linux" ]; then
    PORTA="$(ls /dev/ttyACM* 2>/dev/null | head -1)"
    [ -z "$PORTA" ] && PORTA="$(ls /dev/ttyUSB* 2>/dev/null | head -1)"
fi

if [ -n "$PORTA" ]; then
    echo "Porta detectada: $PORTA"
    read -p "Usar esta porta? [S/n]: " CONFIRMA
    [ "$CONFIRMA" = "n" ] || [ "$CONFIRMA" = "N" ] && PORTA=""
fi

if [ -z "$PORTA" ]; then
    echo "Porta nao detectada automaticamente."
    if [ "$OS_TYPE" = "Darwin" ]; then
        echo "Portas disponiveis (macOS):"
        ls /dev/cu.* 2>/dev/null | grep -v "Bluetooth\|wlan" || echo "  (nenhuma)"
    else
        echo "Portas disponiveis (Linux):"
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  (nenhuma)"
    fi
    read -p "Digite a porta (ex: /dev/ttyACM0): " PORTA
    [ -z "$PORTA" ] && echo "ERRO: nenhuma porta." && exit 1
fi

echo ""
echo "Gravando RX em $PORTA..."
echo ""

$PYTHON -m esptool --chip esp32s3 -p "$PORTA" -b 460800 \
    --before=default_reset --after=hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x0 "$BOOTLOADER" 0x10000 "$APP" 0x8000 "$PARTTABLE"

echo ""
echo ""
echo " SUCESSO! RX gravado e reiniciado."
echo ""
