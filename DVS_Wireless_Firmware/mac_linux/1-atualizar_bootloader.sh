#!/bin/bash
set -e
cd "$(dirname "$0")"
echo "============================================================"
echo " Atualizador de Bootloader - nice!nano (DVS Wireless)"
echo "============================================================"
echo ""
if ! command -v python3 &> /dev/null; then
    echo "ERRO: Python 3 nao esta instalado."
    echo "  Ubuntu/Mint/Debian: sudo apt install python3 python3-pip"
    echo "  Fedora:             sudo dnf install python3 python3-pip"
    echo "  Arch:               sudo pacman -S python python-pip"
    echo "  macOS:              brew install python3"
    read -p "Pressione ENTER para sair..."
    exit 1
fi
echo "OK: Python -> $(python3 --version 2>&1)"
echo ""
if ! command -v adafruit-nrfutil &> /dev/null; then
    echo "AVISO: adafruit-nrfutil nao encontrado. Instalando..."
    pip3 install --user adafruit-nrfutil
    if ! command -v adafruit-nrfutil &> /dev/null; then
        echo ""
        echo "AVISO: comando ainda nao encontrado no PATH."
        echo "No macOS, o pip --user instala em ~/Library/Python/<versao>/bin,"
        echo "que pode nao estar no PATH. Tente:"
        echo "  echo 'export PATH=\"\$HOME/Library/Python/3.9/bin:\$PATH\"' >> ~/.zshrc"
        echo "  source ~/.zshrc"
        echo "(ajuste '3.9' para a versao do seu Python, mostrada acima)"
        echo ""
        echo "No Linux, tente adicionar ~/.local/bin ao PATH:"
        echo "  echo 'export PATH=\$HOME/.local/bin:\$PATH' >> ~/.bashrc"
        echo "  source ~/.bashrc"
        echo "Se der erro de permissao (PEP 668):"
        echo "  pip3 install --user --break-system-packages adafruit-nrfutil"
        read -p "Pressione ENTER para sair..."
        exit 1
    fi
fi
echo "OK: ferramentas encontradas."
echo ""
PACOTE="nice_nano_bootloader-0.10.0_s140_6.1.1.zip"
if [ ! -f "$PACOTE" ]; then
    echo "ERRO: nao encontrei o arquivo \"$PACOTE\" nesta pasta."
    read -p "Pressione ENTER para sair..."
    exit 1
fi
echo "Pacote encontrado: $PACOTE"
echo ""
echo "ANTES DE CONTINUAR:"
echo "  1. Conecte o TX via cabo USB direto no computador"
echo "  2. De um duplo-toque RAPIDO no botao de reset"
echo "  3. Confirme que apareceu \"NICENANO\" no gerenciador de arquivos"
echo "  4. Anote a porta serial listada abaixo:"
echo ""
echo "Portas seriais detectadas:"
OS_TYPE="$(uname -s)"
if [ "$OS_TYPE" = "Darwin" ]; then
    PORTAS=$(ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.usbserial* 2>/dev/null || true)
else
    PORTAS=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
fi
if [ -z "$PORTAS" ]; then
    echo "  Nenhuma porta serial encontrada."
    if [ "$OS_TYPE" = "Linux" ]; then
        echo "  Se a placa aparece mas sem permissao, rode:"
        echo "    sudo usermod -aG dialout \$USER"
        echo "  Depois faca logout e login novamente."
    fi
    read -p "Pressione ENTER para sair..."
    exit 1
fi
echo "$PORTAS" | while read -r p; do echo "  - $p"; done
echo ""
read -p "Digite o caminho completo da porta (ex: /dev/ttyACM0 ou /dev/cu.usbmodem1101): " PORTA
if [ -z "$PORTA" ] || [ ! -e "$PORTA" ]; then
    echo "ERRO: porta invalida."
    read -p "Pressione ENTER para sair..."
    exit 1
fi
if [ ! -w "$PORTA" ]; then
    echo "AVISO: sem permissao de escrita em $PORTA. Usando sudo..."
    SUDO="sudo"
else
    SUDO=""
fi
echo ""
echo "============================================================"
echo " ATENCAO: atualizar o BOOTLOADER e sensivel."
echo " Nao desconecte o cabo USB nem desligue o computador."
echo "============================================================"
echo ""
read -p "Tem certeza que quer continuar? (S/N): " CONFIRMA
if [[ ! "$CONFIRMA" =~ ^[Ss]$ ]]; then
    echo "Operacao cancelada."
    read -p "Pressione ENTER para sair..."
    exit 0
fi
echo ""
echo "Gravando bootloader em $PORTA..."
echo ""
$SUDO adafruit-nrfutil dfu serial --package "$PACOTE" -p "$PORTA" -b 115200 --singlebank
if [ $? -ne 0 ]; then
    echo "ERRO durante a gravacao. Verifique a porta e tente novamente."
else
    echo "SUCESSO! Bootloader atualizado. A placa deve reiniciar sozinha."
fi
echo ""
read -p "Pressione ENTER para sair..."
