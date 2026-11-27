# Robô Sumô 10x10 — ESP32

Robô autônomo 10 cm × 10 cm feito para a disciplina de **Sistemas Embarcados** (Universidade do Vale do Paraíba). A prova é encontrar e **derrubar uma garrafa** dentro de uma arena **totalmente preta** (~65–70 cm) sem sair dela.

**Integrantes:** Luiz Thiago Gonçalves Cassab e Matheus Henrique Rodrigues.

O robô procura o alvo com um sensor ultrassônico, avança para empurrá-lo e usa um sensor infravermelho apontado para o chão para não sair da arena.

## Conteúdo do repositório

| Pasta | O que tem |
|---|---|
| [`hardware/kicad`](hardware/kicad) | Esquemático elétrico no KiCad (revisão R2) e o [PDF exportado](hardware/kicad/Robo10x10.pdf) (2 páginas: diagrama e tabela de ligações) |
| [`firmware/robo-sumo-idf`](firmware/robo-sumo-idf) | **Código final** do robô (ESP-IDF em C, projeto PlatformIO) — o que foi gravado e testado no robô real |
| [`simulacao/wokwi-arduino`](simulacao/wokwi-arduino) | Primeira versão do comportamento em Arduino, com circuito e teste automático no Wokwi |
| [`simulacao/wokwi-idf-v1`](simulacao/wokwi-idf-v1) | Versão anterior do código ESP-IDF, validada no Wokwi |
| [`ferramentas/diagnostico-hc-sr04`](ferramentas/diagnostico-hc-sr04) | Programa de diagnóstico do ultrassônico (mostra o sinal cru do ECHO, motores desligados) |

## Componentes

| Componente | Função |
|---|---|
| ESP32 DevKit **38 pinos** (módulo WROOM-32) | Controlador |
| Ponte H **TB6612FNG** | Aciona os dois motores |
| 2× motor **N20 12 V, 300 rpm** | Tração |
| Sensor ultrassônico **HC-SR04** | Detecta o alvo à frente |
| Sensor infravermelho **TCRT5000** (módulo de 4 pinos) | Detecta a borda da arena (qualquer cor que não seja preta) |
| Conversor **MP1584** (step-down) | Gera 5 V a partir da bateria |
| **Level shifter** (BSS138, 4 canais) | Converte ECHO e D0 de 5 V para 3,3 V |
| Suporte com 3× **18650** em série (3S, 9 a 12,6 V) | Bateria |
| Push-button **com trava** (6 pinos) | Chave liga/desliga da linha de 5 V |

## Alimentação

```
Bateria 3S (9–12,6 V) ──┬──► MP1584 IN+ ──► OUT+ 5,0 V ──► CHAVE ──► 5 V: ESP32 (V5), HC-SR04, TCRT5000, shifter HV
                        │                                                      │
                        └──► Ponte H VM (motores)                     ESP32 3V3 ──► shifter LV, ponte H VCC e STBY
GND comum: bateria −, MP1584, ESP32, sensores, shifter e ponte H
```

- **MP1584 ajustado para 5,0 V com multímetro, sem carga, antes de ligar o resto.**
- Furos do MP1584 na placa: **1, 2, 5, 6 = GND**; **3 = IN+** (bateria + e VM da ponte H); **7 = OUT+ 5 V**; 4 e 8 sem uso.
- GND em estrela: o barramento sai do furo 5 para ESP32, sensores e shifter; o GND da ponte H vai num fio grosso separado até o furo 2, junto do negativo da bateria.
- **Chave:** em série na linha de 5 V (OUT+ → pino do meio → pino lateral → cargas de 5 V). Não tem GND. Desligada, o ESP32 apaga; sem 3V3, o VCC/STBY da ponte H caem e os motores param (o VM continua ligado na bateria).

## Ligações do ESP32

Os pinos aparecem com os dois nomes: o do esquemático (Dxx) e o impresso na placa usada (Gxx).

| ESP32 | Liga em | Observação |
|---|---|---|
| D22/G22 | HC-SR04 TRIG | direto (3,3 V é suficiente para o disparo) |
| D35/G35 | shifter LV1 ← HV1 ← HC-SR04 ECHO | pino só de entrada |
| D34/G34 | shifter LV4 ← HV4 ← TCRT5000 D0 | pino só de entrada; LOW = cor diferente de preto |
| D13/G13 | TB6612 PWMA | motor **direito** (A01/A02) |
| D14/G14 | TB6612 AIN1 | |
| D12/G12 | TB6612 AIN2 | pino de *strapping*: precisa estar em LOW no boot (a ponte H já puxa para baixo) |
| D16/G16 | TB6612 PWMB | motor **esquerdo** (B01/B02) |
| D26/G26 | TB6612 BIN1 | |
| D33/G33 | TB6612 BIN2 | |
| 3V3 | shifter LV, TB6612 VCC e STBY | |
| 5V/V5 | saída da chave (5 V do MP1584) | |

O A0 do TCRT5000 fica **sem ligação** (chega a 5 V). Os canais 2 e 3 do shifter não são usados.

## Cores dos fios

| Ligação | Cor |
|---|---|
| Bateria + / − | vermelho / preto |
| Linha de 5 V | vermelho |
| Linha de 3,3 V | branco |
| GND | preto |
| PWMA / AIN2 / AIN1 | amarelo / verde / branco |
| BIN1 / BIN2 / PWMB | roxo / azul / cinza |

| Sensor | Pino | Na placa | No adaptador |
|---|---|---|---|
| HC-SR04 | VCC | vermelho | marrom |
| | TRIG | preto | verde |
| | ECHO | amarelo | amarelo |
| | GND | preto | laranja |
| TCRT5000 | VCC | vermelho | branco |
| | GND | preto | roxo escuro |
| | D0 | preto | azul escuro |

As cores mudam no conector do adaptador: na montagem, guie-se pelo **nome do pino**, não pela cor.

## Firmware final (`firmware/robo-sumo-idf`)

ESP-IDF em C puro, compilado e gravado com **PlatformIO** (`framework = espidf`). A pinagem fica em `src/pins.h`.

Comportamento (potência global de **50%** — `FATOR_POTENCIA`):

1. **Espera de 5 s** após ligar; nos últimos 400 ms já lê o ultrassônico com o robô parado. Se houver alvo a até 50 cm, ataca direto.
2. **Busca:** gira no próprio eixo (~2,5 s, alternando o lado) e dá um passo curto para frente.
3. **Alvo a até 50 cm:** se estava girando, aplica freio (*short brake* da TB6612) para não passar do alvo, confirma a leitura e **ataca reto** com rampa de partida. Se perdeu o alvo no freio, faz um contragiro curto para recentralizar.
4. **Ponto cego:** abaixo de 16 cm continua empurrando por até 450 ms mesmo sem eco (alvo encostado).
5. **Borda** (TCRT5000 vê algo que não é preto por 3 leituras seguidas): **freia**, dá **ré** (280 ms) e **gira** de volta para o centro; se ainda estiver na borda, repete a ré.

O HC-SR04 é lido **por interrupção** no pino ECHO, sem travar o loop, com proteção contra ECHO preso em nível alto.

### Gravar

1. Abra a pasta `firmware/robo-sumo-idf` no VS Code com a extensão PlatformIO.
2. **Tire as baterias (ou suspenda as rodas).** A chave só corta o 5 V: com o USB ligado, o 3V3 do ESP32 mantém a ponte H ativa e os motores giram.
3. Conecte o ESP32 no USB, use **Upload** e depois **Monitor** (115200). A primeira linha mostra `POTENCIA 50%`; depois aparece o estado, a distância e a borda a cada 200 ms.

Ajustes rápidos ficam no topo do `main.c`: `FATOR_POTENCIA`, `DISTANCIA_ATAQUE_CM`, tempos das manobras e `INVERTER_MOTOR_DIR/ESQ` (se uma roda girar ao contrário).

## Calibração do infravermelho

O módulo TCRT5000 compara o reflexo com um limite ajustado no **trimpot**; o código só lê o D0.

1. Monte o sensor na altura final (uns 3 a 8 mm do chão).
2. Sem baterias, com o ESP32 no USB, apoie o robô na arena preta.
3. Gire o trimpot até o **LED de sinal apagar no preto**, volte ao ponto em que ele acende e dê ~1/8 de volta de folga.
4. Passe sobre a borda/outra cor: o LED deve **acender**. Calibre no local da prova (luz forte interfere).

## Simulação no Wokwi

As pastas em `simulacao/` têm o circuito (`diagram.json`) com o ESP32 DevKitC V4 (mesma ordem de pinos da placa real). O Wokwi não tem TB6612, MP1584, level shifter nem TCRT5000: os sinais da ponte H aparecem em LEDs e o infravermelho é uma chave deslizante (direita = preto, esquerda = borda). Clique no HC-SR04 durante a simulação para mudar a distância.

- **Navegador:** crie um projeto ESP32 em wokwi.com e cole o código e o `diagram.json`.
- **Wokwi CLI** (teste automático): compile para `build/` e rode `wokwi-cli --scenario teste.yaml .` com a variável `WOKWI_CLI_TOKEN` definida.
  - `wokwi-arduino`: copie `sketch.ino` para uma pasta chamada `sketch` e compile com `arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build`.
  - `wokwi-idf-v1`: rode `compilar_wokwi.ps1` (compila o código ESP-IDF pelo Arduino CLI só para simular).

## Diagnóstico do ultrassônico

`ferramentas/diagnostico-hc-sr04` mantém os motores desligados e imprime, a cada 200 ms, o nível do ECHO antes do disparo, o tempo até ele subir e a largura do pulso. Serve para separar problema de montagem (algo a menos de 3 cm do sensor, cilindros encostados no chassi, TRIG/ECHO trocados) de problema de código.

## Segurança

- Ajuste o MP1584 para 5,0 V **antes** de ligar qualquer carga.
- Para gravar ou testar pelo USB, **tire as baterias**.
- Nunca ligue o VM da ponte H no 5 V/3,3 V nem o VCC na bateria: 12 V no 3V3 queima o ESP32.
- O A0 do TCRT5000 não pode ir direto ao ESP32 (5 V).
