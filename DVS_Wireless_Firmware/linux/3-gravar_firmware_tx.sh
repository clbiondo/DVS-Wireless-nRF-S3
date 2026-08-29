#!/bin/bash
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
TX_DIR="$DIR/../tx"
echo ""
echo " Gravador de Firmware - TX nice!nano (DVS Wireless)"
echo ""
echo ""

OS_TYPE="$(uname -s)"
UF2FILE="$(find "$TX_DIR" -maxdepth 1 -name "*.uf2" | head -1)"
if [ -z "$UF2FILE" ]; then
    echo "ERRO: nenhum .uf2 em $TX_DIR/"
    exit 1
fi
echo "Firmware: $(basename "$UF2FILE")"
echo ""
echo "ANTES DE CONTINUAR:"
echo "  1. Conecte o TX via cabo USB"
echo "  2. Duplo-toque RAPIDO no botao de reset"
echo "  3. Espere NICENANO aparecer"
echo ""
read -p "Fez o duplo-toque? Aperte ENTER..."
echo ""
echo "Procurando NICENANO..."
NICE_PATH=""
TENTATIVAS=0
while [ $TENTATIVAS -lt 15 ]; do
    TENTATIVAS=$((TENTATIVAS + 1))
    if [ "$OS_TYPE" = "Darwin" ]; then
        for vol in /Volumes/*; do
            if [ -d "$vol" ]; then
                LABEL="$(diskutil info "$vol" 2>/dev/null | grep 'Volume Name' | sed 's/.*Volume Name: *//')"
                [ "$LABEL" = "NICENANO" ] && NICE_PATH="$vol" && break
            fi
        done
    elif [ "$OS_TYPE" = "Linux" ]; then
        for base in /media "/run/media/$(whoami)" /media/$(whoami); do
            [ -d "$base/NICENANO" ] && NICE_PATH="$base/NICENANO" && break
        done
        if [ -z "$NICE_PATH" ]; then
            NICE_LABEL="$(lsblk -o LABEL,MOUNTPOINT -n 2>/dev/null | grep '^NICENANO' | awk '{print $2}')"
            [ -n "$NICE_LABEL" ] && NICE_PATH="$NICE_LABEL"
        fi
    fi
    [ -n "$NICE_PATH" ] && break
    sleep 2
done
if [ -z "$NICE_PATH" ]; then
    echo "ERRO: NICENANO nao encontrado apos 30s."
    echo "Verifique cabo, duplo-toque e se a unidade aparece."
    read -p "Pressione ENTER para sair..."
    exit 1
fi
echo "Unidade: $NICE_PATH"
echo ""
echo "Copiando $(basename "$UF2FILE")..."
cp "$UF2FILE" "$NICE_PATH/"
echo ""
echo ""
echo " SUCESSO! TX gravado. Reinicia em segundos."
echo ""
read -p "Pressione ENTER para sair..."
