# DVS Wireless nRF+S3

> ⚠️ **Aviso**: este é um projeto amador, open source, feito por hobby e aprendizado — não é um produto comercial polido nem testado em escala. Se você busca algo **profissional, robusto e sem falhas, pronto pra uso em produção sem surpresas**, a recomendação honesta é comprar um **Phase DJ** de verdade. Eu mesmo tenho um Phase DJ e acho o produto excelente. Este projeto existe como alternativa DIY de baixo custo para quem gosta de eletrônica, quer aprender, ou está disposto a lidar com as limitações e ajustes finos de um sistema construído em casa.

Sistema de timecode sem fio (Digital Vinyl System) para DJs, desenvolvido como alternativa de baixo custo e código aberto ao Phase DJ e a sistemas de timecode via cabo (Serato DVS, Traktor Scratch, Rekordbox DVS, MixVibes).

Cada disco de vinil recebe um transmissor (**TX**) pequeno e sem fio, alimentado por bateria, que lê a rotação do prato por giroscópio e transmite essa informação por rádio 2,4GHz para um receptor (**RX**) fixo, que gera o sinal de timecode analógico e entrega para a placa de som do computador — exatamente como um vinil de timecode físico faria.

Este projeto é baseado no trabalho original de [Felipe Alme](https://github.com/FelipeAlme/DVS-Wireless-DIY-DJ-System) e evoluiu de forma independente ao longo de meses de desenvolvimento, com uma arquitetura de hardware e firmware diferente (rádio nRF24L01 em vez de ESP-NOW, giroscópio LSM6DS3 em vez de MPU6050) e diversas funcionalidades adicionais.

---

## Funcionalidades

- **Dois decks independentes**, cada um com seu próprio TX e seu próprio canal de rádio dedicado.
- **Quatro formatos de portadora suportados**, selecionáveis por deck em tempo real — reproduzem a frequência e a relação de fase esperadas por cada plataforma (não o timecode bit-a-bit autêntico — ver "Como funciona" abaixo):
  - Serato CV02 (1000Hz, 90°)
  - Traktor MK1 (2000Hz, 270°, canais invertidos)
  - MixVibes V2 (1300Hz, 270°)
  - Pioneer Rekordbox DVS (1000Hz, 90°)
- **Pareamento sem fio** entre TX e RX por canal e endereço de rádio configuráveis, sem necessidade de reflashar firmware — basta segurar o botão do dock por 3 segundos.
- **Baixo consumo de energia no TX** — com uma bateria de 50mAh, a autonomia é de aproximadamente 5 horas de uso contínuo.
- **Troca de formato de timecode com um toque rápido** no mesmo botão — útil para trocar de software de DJ sem precisar mexer em configuração nenhuma.
- **Indicação visual por LED** de status de pareamento e do formato de timecode selecionado. _(O comportamento exato do LED em relação a força de sinal/perda de pacotes ainda não está confirmado nesta versão — ver observação na seção de hardware.)_

  | Situação | Cor do LED |
  |---|---|
  | Repouso / sistema rodando normal | Azul fraco |
  | Pareamento em andamento | Roxo |
  | Pareamento confirmado | Verde |
  | Pareamento falhou | Vermelho |
  | Gravação/SWD ativa | Ciano |
  | Formato selecionado: Serato CV02 | Verde |
  | Formato selecionado: Traktor MK1 | Azul |
  | Formato selecionado: MixVibes V2 | Roxo |
  | Formato selecionado: Pioneer RB | Amarelo |

  _(O flash de cor do formato dura ~800ms após o toque rápido no botão, voltando em seguida ao estado de repouso.)_
- **Um único firmware de TX** para qualquer disco — não é necessário compilar/gravar firmware diferente por deck; o pareamento grava a identidade (canal + endereço) na memória não-volátil do próprio TX.

---

## Como funciona (visão geral)

1. O TX fica preso ao centro do prato (ou a um adaptador que gira junto com o disco), lendo a velocidade angular pelo giroscópio a alta taxa de amostragem.
2. A cada leitura, o TX transmite a velocidade instantânea por rádio (protocolo Enhanced ShockBurst do nRF24L01), endereçada ao canal do deck em que foi pareado.
3. O RX recebe os pacotes, aplica um pequeno ajuste de calibração por deck (para compensar variação de tolerância entre giroscópios), e sintetiza em tempo real uma **portadora senoidal pura em quadratura** (seno/cosseno) — variando sua frequência e sentido de fase de acordo com a velocidade e a direção de rotação recebidos.

**Importante sobre o tipo de sinal gerado**: este sistema reproduz a frequência e a relação de fase da portadora de cada formato suportado (o suficiente para o software de DJ calcular velocidade e direção), mas **não** reproduz o padrão de ruído pseudoaleatório que os formatos de timecode reais (Serato CV02, Traktor MK1, etc.) usam para codificar posição absoluta no disco. Ou seja, tecnicamente o sinal gerado funciona de forma análoga ao **Phase DJ** — um timecode relativo, baseado em velocidade/direção, sem posição absoluta — e não é uma reprodução bit-a-bit do timecode autêntico de cada plataforma. Isso é intencional e é justamente o que o software de DJ reconhece (ver nota abaixo).

4. Esse sinal de áudio sai por um DAC dedicado por deck (dois canais estéreo independentes) e é conectado à entrada de linha da placa de som — o software de DJ interpreta esse sinal como se fosse o de um controlador Phase DJ físico.

**Nota sobre reconhecimento pelo software**: softwares de DJ como VirtualDJ e algoriddim djay Pro reconhecem esse sinal como sendo do tipo **Phase DJ** — ou seja, um timecode baseado em velocidade/direção relativa (sem posição absoluta codificada no sinal), diferente do timecode tradicional de vinil (Serato, Traktor Scratch, Rekordbox), que carrega posição absoluta real via padrão de ruído pseudoaleatório no próprio disco de vinil de controle.

O sistema **não grava nem processa áudio de música** — ele só gera o sinal de controle (timecode). A faixa de áudio real continua sendo tocada digitalmente pelo software de DJ, como em qualquer configuração DVS tradicional.

---

## Uso no dia a dia

### Pareamento inicial (uma vez por disco/deck)

1. Encaixa o TX no dock de pareamento correspondente ao deck desejado (dock 1 ou dock 2).
2. Segura o botão daquele dock por 3 segundos.
3. O LED indica sucesso do pareamento. O TX grava o canal/endereço designado e reinicia automaticamente.
4. A partir daí, esse TX específico está associado àquele deck — pode ser levado pra qualquer toca-discos, contanto que fique dentro do alcance de rádio do RX.

### Troca de formato de timecode

Um toque rápido (não segurado) no botão do dock alterna, em sequência, entre os 4 formatos suportados — a mudança se aplica aos dois decks simultaneamente, e o LED pisca brevemente numa cor indicando o formato selecionado.

### Uso normal

Com o TX pareado e encaixado no prato, é só ligar o toca-discos e o software de DJ em modo timecode/DVS — o sistema já está pronto para uso, sem nenhuma outra configuração necessária.

---

## Estrutura deste repositório

```
/firmware/rx/       — Firmware do receptor (ESP32-S3, ESP-IDF)
/firmware/tx/        — Firmware do transmissor (nRF52840, Zephyr/NCS)
/hardware/           — Esquemas elétricos e diagramas de ligação
/docs/               — Instruções de montagem e este manual
```

## Créditos

Baseado no projeto original [FelipeAlme/DVS-Wireless-DIY-DJ-System](https://github.com/FelipeAlme/DVS-Wireless-DIY-DJ-System). Agradecimentos ao Felipe pela base conceitual do sistema.

## Licença

Este projeto é distribuído sob a licença **GPL-3.0** — veja o arquivo [`LICENSE`](LICENSE) para o texto completo. Em resumo: você pode usar, estudar, modificar e redistribuir este projeto livremente, mas qualquer trabalho derivado distribuído publicamente deve continuar sob a mesma licença (código aberto).
