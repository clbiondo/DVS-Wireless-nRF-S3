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
    if python3 -c "import sys; exit(0 if sys.prefix != sys.base_prefix else 1)" 2>/dev/null; then
        # Dentro de um ambiente virtual (detectado via sys.prefix)
        pip3 install adafruit-nrfutil
    else
        if ! pip3 install --user adafruit-nrfutil 2>/tmp/dvs_pip_err.log; then
            if grep -q "externally-managed-environment" /tmp/dvs_pip_err.log; then
                echo "Ambiente Python gerenciado pelo sistema (PEP 668) - tentando com --break-system-packages..."
                pip3 install --user --break-system-packages adafruit-nrfutil
            else
                cat /tmp/dvs_pip_err.log
            fi
        fi
        rm -f /tmp/dvs_pip_err.log
    fi
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
echo ""
read -p "Feito? Aperte ENTER para procurar a porta..."
echo ""

OS_TYPE="$(uname -s)"
detectar_portas() {
    if [ "$OS_TYPE" = "Darwin" ]; then
        ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.usbserial* 2>/dev/null || true
    else
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true
    fi
}

echo "Procurando porta serial..."
PORTAS_ENCONTRADAS=($(detectar_portas))
PORTA=""

if [ ${#PORTAS_ENCONTRADAS[@]} -eq 0 ]; then
    echo "Nenhuma porta serial encontrada."
    if [ "$OS_TYPE" = "Linux" ]; then
        echo "Se a placa aparece mas sem permissao, rode:"
        echo "  sudo usermod -aG dialout \$USER"
        echo "Depois faca logout e login novamente."
    fi
    read -p "Digite a porta manualmente (ex: /dev/ttyACM0 ou /dev/cu.usbmodemXXXX): " PORTA
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

if [ ! -e "$PORTA" ]; then
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
