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

# ── Deteccao automatica de porta serial (Linux + macOS) ──────────────────
detectar_portas() {
    if [ "$OS_TYPE" = "Darwin" ]; then
        ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.usbserial* 2>/dev/null || true
    else
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true
    fi
}

PORTAS_ENCONTRADAS=($(detectar_portas))
PORTA=""

if [ ${#PORTAS_ENCONTRADAS[@]} -eq 0 ]; then
    echo "Nenhuma porta detectada automaticamente."
    if [ "$OS_TYPE" = "Darwin" ]; then
        echo "Portas disponiveis (macOS):"
        ls /dev/cu.* 2>/dev/null | grep -v "Bluetooth\|wlan\|debug-console" || echo "  (nenhuma)"
    else
        echo "Portas disponiveis (Linux):"
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  (nenhuma)"
        echo "Se a placa aparece mas sem permissao, rode:"
        echo "  sudo usermod -aG dialout \$USER   (e faca logout/login)"
    fi
    read -p "Digite a porta manualmente (ex: /dev/ttyACM0): " PORTA
    [ -z "$PORTA" ] && echo "ERRO: nenhuma porta." && exit 1
elif [ ${#PORTAS_ENCONTRADAS[@]} -eq 1 ]; then
    PORTA="${PORTAS_ENCONTRADAS[0]}"
    echo "Porta detectada automaticamente: $PORTA"
else
    echo "Multiplas portas encontradas:"
    i=1
    for p in "${PORTAS_ENCONTRADAS[@]}"; do
        echo "  $i) $p"
        i=$((i+1))
    done
    read -p "Escolha o numero da porta: " ESCOLHA
    if [[ "$ESCOLHA" =~ ^[0-9]+$ ]] && [ "$ESCOLHA" -ge 1 ] && [ "$ESCOLHA" -le ${#PORTAS_ENCONTRADAS[@]} ]; then
        PORTA="${PORTAS_ENCONTRADAS[$((ESCOLHA-1))]}"
    else
        echo "ERRO: escolha invalida."
        exit 1
    fi
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
read -p "Pressione ENTER para sair..."
