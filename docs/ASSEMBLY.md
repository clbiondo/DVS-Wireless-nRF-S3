# Instruções de montagem — DVS Wireless nRF+S3

Este guia assume que você já tem os componentes eletrônicos em mãos e conhecimento básico de solda. Para a lista completa de pinos, consulte [`hardware/HARDWARE.md`](../hardware/HARDWARE.md).

## Lista de materiais (por sistema completo, 2 decks)

**Receptor (1 unidade):**
- 1x placa ESP32-S3 (com pelo menos 20 GPIOs livres de uso geral)
- 2x módulo nRF24L01+ (recomendado: versão com regulador de tensão embutido, mais tolerante a ruído de alimentação)
- 2x módulo DAC PCM5102 (breakout)
- 2x botão de pressão (momentâneo, normalmente aberto)
- 1x LED endereçável (WS2812 ou similar) — **[confirmar tipo exato antes de montar, ver HARDWARE.md]**
- Fios, conectores para o dock (a definir conforme mecânica escolhida)

**Transmissor (1 unidade por deck/prato):**
- 1x placa Adafruit Feather nRF52840 (ou compatível)
- 1x módulo giroscópio LSM6DS3 (breakout)
- 1x bateria LiPo com conector JST-PH 2 pinos (verificar polaridade compatível com a Feather)
- Case/suporte para fixar no prato do toca-discos (peso e balanceamento importam para não afetar a rotação)

## Montagem do TX (repetir para cada deck)

1. Solde os 5 fios do LSM6DS3 na Feather nRF52840, conforme a tabela da seção 2.1 de `HARDWARE.md` (SCK, MOSI, MISO, CS, IRQ, mais VCC e GND).
2. Verifique a orientação do módulo giroscópio antes de fixar definitivamente — a orientação dos eixos X/Y/Z afeta a leitura de rotação, e pode ser necessário ajustar isso no firmware caso o sensor seja montado em orientação diferente da usada no desenvolvimento original.
3. Conecte a bateria LiPo ao conector JST da Feather.
4. Grave o firmware do TX (ver seção de firmware abaixo) antes de fechar a carcaça.
5. Monte tudo em um case compacto e leve, fixado ao centro do prato (ou a um adaptador que gire junto com o disco de vinil).

## Montagem do RX

1. Solde os dois módulos nRF24L01+ na placa ESP32-S3, conforme as seções 1.1 e 1.2 de `HARDWARE.md`. **Atenção à alimentação em 3.3V, nunca 5V.**
2. Solde os dois módulos PCM5102, conforme as seções 1.3 e 1.4.
3. Instale os dois botões de pareamento (GPIO40 e GPIO41), ligando um terminal ao GPIO correspondente e o outro ao GND (o firmware já usa pull-up interno, não é necessário resistor externo).
4. Instale o LED de status no GPIO48.
5. Prepare os conectores/pogo pins de cada dock, expondo os 3 sinais necessários (UART TX, UART RX, GND) para contato com o TX quando encaixado.
6. Conecte as saídas de áudio (L/R) de cada PCM5102 a um cabo com conector adequado à entrada de linha da sua placa de som (geralmente P2 estéreo ou RCA, dependendo do módulo DAC).

## Firmware

### Gravando via Windows (recomendado para a maioria dos usuários)

Este projeto disponibiliza uma pasta `DVS_Wireless_Firmware/windows/` com os **binários já compilados** (RX e TX) prontos para gravar, sem precisar instalar ESP-IDF nem NCS/Zephyr.

**Pré-requisito**: [Python](https://python.org) instalado no Windows (necessário para o `esptool`, usado na gravação do RX). Os scripts **não instalam o Python automaticamente** — se ele não estiver presente, o script `.bat` avisa e interrompe, pedindo para você instalar manualmente antes de rodar de novo. A biblioteca `esptool` em si **é instalada automaticamente** pelo script (via `pip`), caso ainda não esteja presente.

**Gravando o RX** (`gravar_firmware_rx.bat`):
1. Conecte o ESP32-S3 (RX) ao computador via USB.
2. Dê duplo-clique em `gravar_firmware_rx.bat`.
3. O script verifica se o Python está instalado e, se necessário, instala o `esptool`.
4. Digite a porta COM correspondente (visível no Gerenciador de Dispositivos do Windows) quando solicitado.
5. Aguarde a gravação — o script grava bootloader, tabela de partições e o firmware principal automaticamente.

**Gravando o TX** (`gravar_firmware_tx.bat`) — atualiza também o bootloader:
1. Conecte o TX (nice!nano/nRF52840) via cabo USB.
2. Dê duplo-toque rápido no botão de reset da placa para entrar em modo bootloader (a placa deve aparecer como uma unidade removível chamada `NICENANO`).
3. Dê duplo-clique em `gravar_firmware_tx.bat` e siga as instruções na tela — o script detecta a unidade `NICENANO` automaticamente e copia o firmware `.uf2` para ela.
4. Esse mesmo processo **também atualiza o bootloader do TX**, já que a pasta de firmware inclui os arquivos necessários para isso.

### Gravando via Linux ou macOS (testado e funcional nos dois sistemas)

Existem duas pastas com **binários já compilados** e scripts prontos, uma para cada sistema:
- `DVS_Wireless_Firmware/linux/` — scripts `.sh`, rodados pelo terminal
- `DVS_Wireless_Firmware/mac/` — scripts `.command`, prontos para **duplo-clique direto no Finder**, sem precisar converter nada manualmente

O conteúdo dos scripts é idêntico entre as duas pastas (cada script já detecta sozinho se está rodando em Linux ou macOS e ajusta portas seriais/detecção de unidade automaticamente) — a única diferença é a extensão do arquivo, para funcionar com o clique duplo em cada sistema.

> **Status por sistema operacional**: testado e confirmado funcional tanto em Linux quanto em macOS, incluindo a detecção automática de porta serial.

**Detecção automática de porta**: os três scripts agora detectam a porta serial sozinhos. Se só existir uma porta candidata conectada, o script já usa ela direto, sem perguntar nada. Se houver mais de uma, aparece uma lista numerada para você escolher (`1) /dev/ttyACM0`, `2) /dev/ttyACM1`, etc.) — só digite o número. Só é preciso digitar o caminho manualmente se nenhuma porta for detectada.

**macOS — primeira execução de um `.command`**: o Gatekeeper do macOS bloqueia a primeira execução de qualquer script baixado da internet. Se aparecer um aviso ("não foi possível verificar..."), clique em **"OK"** (não em "Mover para o Lixo"), depois vá em **Preferências do Sistema (ou Ajustes do Sistema) → Privacidade e Segurança**, role até a seção de segurança, e clique em **"Abrir Mesmo Assim"** ao lado do aviso referente ao script. Depois disso, o duplo-clique funciona normalmente.

**macOS — dica sobre `adafruit-nrfutil`**: se o script de bootloader avisar "adafruit-nrfutil nao encontrado" mesmo depois de instalado, é porque o `pip install --user` no macOS instala o executável em `~/Library/Python/<versão>/bin`, que não fica no PATH por padrão. O próprio script mostra o comando exato para corrigir isso quando detecta o problema.

**Pré-requisito**: Python 3 instalado (`sudo apt install python3 python3-pip` no Ubuntu/Mint/Debian, `sudo dnf install python3 python3-pip` no Fedora, `brew install python3` no macOS). Assim como no Windows, o script **não instala o Python automaticamente** — se não encontrar, ele avisa e para. A biblioteca `esptool` **é instalada automaticamente** via pip, se ainda não estiver presente.

**Passo 1 — Atualizar o bootloader do TX** (`1-atualizar_bootloader.sh`) — **obrigatório na primeira vez com uma placa nova**:

Placas nice!nano novas de fábrica costumam vir com o bootloader **desatualizado**, o que impede a gravação normal do firmware. **Antes de gravar o firmware do TX pela primeira vez numa placa nova, é necessário atualizar o bootloader** com este script. Esse mesmo script também serve para recuperação, caso o firmware do TX trave/corrompa depois de já configurado.

1. Conecte o TX, dê duplo-toque rápido no botão de reset para entrar em modo bootloader.
2. No Linux, rode `./1-atualizar_bootloader.sh` no terminal; no macOS, dê duplo-clique em `1-atualizar_bootloader.command`. Siga as instruções — o script instala `adafruit-nrfutil` automaticamente se necessário, detecta a porta serial, e pede confirmação antes de prosseguir (é uma operação sensível — **não desconecte o cabo USB durante o processo**).
3. Depois de concluído com sucesso, a placa está pronta para receber o firmware normalmente (passo 3 abaixo).

**Passo 2 — Gravando o RX** (`2-gravar_firmware_rx.sh`):
1. Conecte o ESP32-S3 (RX) via USB.
2. No Linux, rode `./2-gravar_firmware_rx.sh` no terminal; no macOS, dê duplo-clique em `2-gravar_firmware_rx.command`.
3. O script detecta automaticamente a porta serial (tenta `/dev/ttyACM*`/`/dev/ttyUSB*` no Linux, `/dev/cu.usbmodem*`/`/dev/cu.SLAB_USBtoUART*`/`/dev/cu.usbserial*` no macOS) e pergunta se deseja usá-la — ou permite digitar manualmente.
4. Aguarde a gravação automática do bootloader, tabela de partições e firmware principal.

**Passo 3 — Gravando o TX** (`3-gravar_firmware_tx.sh`):
1. Conecte o TX via USB e dê duplo-toque rápido no botão de reset para entrar em modo bootloader.
2. No Linux, rode `./3-gravar_firmware_tx.sh` no terminal; no macOS, dê duplo-clique em `3-gravar_firmware_tx.command`. Siga as instruções — o script procura a unidade `NICENANO` automaticamente (via `diskutil` no macOS, via `/media`, `/run/media` ou `lsblk` no Linux) e copia o firmware.

Em ambos os sistemas, se aparecer erro de permissão na porta serial no Linux, rode `sudo usermod -aG dialout $USER`, faça logout e login novamente.

### Gravando via linha de comando (ESP-IDF / NCS) — para desenvolvimento

#### Gravando o RX (compilando do código-fonte)

1. Instale o ESP-IDF (v5.3 ou compatível) conforme a documentação oficial da Espressif.
2. Clone este repositório e entre em `firmware/rx/`.
3. `idf.py build`
4. `idf.py -p <porta_serial> flash`

#### Gravando o TX (compilando do código-fonte)

1. Instale o nRF Connect SDK (NCS) e configure o ambiente West conforme a documentação oficial da Nordic.
2. Clone este repositório e entre em `firmware/tx/`.
3. `west build -b adafruit_feather_nrf52840`
4. Grave via arrastar-e-soltar o arquivo `.uf2` gerado na unidade USB que aparece ao colocar a placa em modo bootloader (double-tap no botão reset).

**Importante**: o mesmo firmware de TX serve para qualquer deck — não compile versões diferentes por deck. A identidade (canal/endereço) é definida depois, durante o pareamento físico.

## Primeiro pareamento

1. Ligue o RX (alimentação USB).
2. Ligue o TX (deve ligar automaticamente ao sair do estado de repouso, ou pressione o botão de energia da Feather, se aplicável).
3. Encaixe o TX no dock correspondente ao deck desejado.
4. Segure o botão daquele dock por 3 segundos.
5. Aguarde a confirmação visual no LED — o TX deve reiniciar automaticamente após o pareamento bem-sucedido.
6. Repita para o segundo deck, se aplicável.

## Teste final

1. Conecte as saídas de áudio do RX às entradas de linha da placa de som.
2. Configure seu software de DJ (Serato, Traktor, Rekordbox, MixVibes) para reconhecer timecode nas entradas correspondentes, no formato configurado no sistema (padrão: Serato CV02).
3. Encaixe o TX pareado no prato do toca-discos, ligue o motor, e verifique se o software reconhece o timecode e reproduz a faixa corretamente, incluindo variação de pitch e direção (scratch/reverso).

Se o software não reconhecer o timecode, verifique:
- Formato de timecode selecionado (toque rápido no botão do dock alterna entre os 4 formatos suportados).
- Nível de sinal de saída (alguns softwares têm um medidor de qualidade de timecode na tela de configuração).
- Se o LED de status do dock indica pareamento ativo (sinal de rádio chegando).
