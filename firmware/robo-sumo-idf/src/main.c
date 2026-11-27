/*
 * Robô Sumô Autônomo - ESP32 (ESP-IDF / C puro)
 * Pinagem definida em src/pins.h
 * Arquitetura:
 *  - Leitura do HC-SR04 por interrupção (IRAM_ATTR + spinlock atômico)
 *  - Sensor de borda TCRT5000 com filtro de debounce
 *  - Varredura no eixo com freio eletromagnético (Short Brake) e contragiro
 *  - Rampa suave de partida nos motores e manutenção de empurrão no impacto
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "pins.h"

static const char *TAG = "ROBO_SUMO";

// =========================================================================
// 1. CONFIGURAÇÃO PWM (LEDC - 8 bits: 0 a 255, 5 kHz)
// =========================================================================
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_RES         LEDC_TIMER_8_BIT
#define LEDC_FREQ_HZ     5000
#define LEDC_CH_DIR      LEDC_CHANNEL_0
#define LEDC_CH_ESQ      LEDC_CHANNEL_1

// =========================================================================
// 2. CONFIGURAÇÕES DE CALIBRAÇÃO (POTÊNCIA 50% | ARENA 65-70 CM)
// =========================================================================
// Fator global de escala sobre o PWM máximo dos motores (0.50f = 50%)
#define FATOR_POTENCIA            0.50f

// Padrão TCRT5000 (LM393): 0 = linha branca, 1 = arena preta
#define NIVEL_BORDA_BRANCA        0
#define DEBOUNCE_BORDA_AMOSTRAS   3     // 3 leituras consecutivas (30 ms)

// Inversão de polaridade dos motores por software
#define INVERTER_MOTOR_DIR        false
#define INVERTER_MOTOR_ESQ        false

// Alcance do sensor ultrassônico (em cm)
#define DISTANCIA_ATAQUE_CM       50.0f // Alcance para detecção e travamento
#define DISTANCIA_PONTO_CEGO_CM   16.0f // Limiar para ativar inércia de impacto

// Espera regulamentar após energização (5 segundos)
#define TEMPO_ESPERA_INICIAL_MS   5000
#define TEMPO_PRE_SCAN_MS         400   // Pré-leitura do HC-SR04 com motores parados

// Velocidades PWM (0 a 255) escaladas por FATOR_POTENCIA
#define VEL_ATAQUE_INICIAL        ((int)(200 * FATOR_POTENCIA)) // 100 em 50%
#define VEL_ATAQUE                ((int)(255 * FATOR_POTENCIA)) // 127 em 50%
#define VEL_BUSCA                 ((int)(160 * FATOR_POTENCIA)) // 80 em 50%
#define VEL_GIRO                  ((int)(170 * FATOR_POTENCIA)) // 85 em 50%
#define VEL_CORRECAO              ((int)(150 * FATOR_POTENCIA)) // 75 em 50%
#define VEL_RE_INICIAL            ((int)(180 * FATOR_POTENCIA)) // 90 em 50%
#define VEL_RE_BORDA              ((int)(220 * FATOR_POTENCIA)) // 110 em 50%

// Tempos das manobras, rampas e frenagens (em ms)
#define TEMPO_RAMPA_ATAQUE_MS     45    // Suaviza o pico de corrente na arrancada
#define TEMPO_RAMPA_RE_MS         35    // Suaviza o pico de corrente na ré
#define TEMPO_INERCIA_IMPACTO_MS  450   // Mantém empurrão no contato/ponto cego
#define TEMPO_TOLERANCIA_FALHA_MS 160   // Ignora falhas isoladas de eco no ataque
#define TEMPO_FREIO_ALVO_MS       65    // Tempo mínimo de Short Brake ao ver alvo
#define TEMPO_FREIO_ALVO_MAX_MS   140   // Teto de espera pela leitura pós-freio
#define TEMPO_MIN_GIRO_FREIO_MS   70    // Só aplica freio se já ganhou embalo
#define TEMPO_CORRECAO_ALVO_MS    450   // Contragiro para recentralizar o alvo
#define TEMPO_GIRO_BUSCA_MS       2500  // Varredura ampla (~540°) no próprio eixo
#define TEMPO_AVANCO_BUSCA_MS     300   // Passo curto de reposicionamento
#define TEMPO_FREIO_BORDA_MS      55    // Freio imediato ao detectar a borda
#define TEMPO_RE_BORDA_MS         280   // Ré curta segura na borda
#define TEMPO_GIRO_BORDA_MS       560   // Giro de retorno para o interior da arena

// Parâmetros do HC-SR04 (baseados no datasheet oficial)
#define INTERVALO_ULTRASSOM_MS    60    // Ciclo mínimo entre disparos
#define TIMEOUT_ECHO_MS           45    // Acima do timeout interno (~38 ms)
#define TIMEOUT_ECHO_TRAVADO_MS   120   // Recuperação de pino ECHO preso em HIGH
#define ECHO_MIN_US               117   // ~2,0 cm
#define ECHO_MAX_US               23500 // ~400 cm

#define LOOP_DELAY_TICKS          (pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1)

// =========================================================================
// 3. MÁQUINA DE ESTADOS E VARIÁVEIS COMPARTILHADAS
// =========================================================================
typedef enum {
    BUSCA_GIRAR,
    BUSCA_AVANCAR,
    FREIO_ALVO,
    CORRECAO_ALVO,
    ATACAR,
    FUGA_BORDA_FREIO,
    FUGA_BORDA_RE,
    FUGA_BORDA_GIRO
} estado_robo_t;

static estado_robo_t estado_atual = BUSCA_GIRAR;
static uint32_t tempo_inicio_estado = 0;
static bool girar_para_esquerda = true;
static bool leitura_nova_no_estado = false;

// Controle do sensor de borda com debounce
static uint8_t contador_borda = 0;

// Controle do ultrassom (spinlock garante leitura atômica de 64 bits)
static portMUX_TYPE echo_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t echo_inicio_us = 0;
static volatile int64_t echo_duracao_us = 0;
static volatile bool echo_pronto = false;

static uint32_t ultimo_ping_ms = 0;
static uint32_t ping_atual_ms = 0;
static bool aguardando_echo = false;
static float distancia_atual = 999.0f;

// =========================================================================
// 4. INTERRUPÇÃO DO PINO ECHO (IRAM_ATTR + SPINLOCK)
// =========================================================================
static void IRAM_ATTR echo_isr_handler(void *arg) {
    (void)arg;
    int64_t agora_us = esp_timer_get_time();
    int nivel = gpio_get_level(ECHO);

    portENTER_CRITICAL_ISR(&echo_mux);
    if (nivel == 1) {
        echo_inicio_us = agora_us;
    } else if (echo_inicio_us > 0) {
        echo_duracao_us = agora_us - echo_inicio_us;
        echo_inicio_us = 0;
        echo_pronto = true;
    }
    portEXIT_CRITICAL_ISR(&echo_mux);
}

// =========================================================================
// 5. FUNÇÕES AUXILIARES E HARDWARE
// =========================================================================
static uint32_t millis_agora(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int limitar(int valor, int minimo, int maximo) {
    if (valor < minimo) return minimo;
    if (valor > maximo) return maximo;
    return valor;
}

static void mudar_estado(estado_robo_t novo_estado) {
    estado_atual = novo_estado;
    tempo_inicio_estado = millis_agora();
    leitura_nova_no_estado = false;
}

static void configurar_hardware(void) {
    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL << R_IN1) |
                        (1ULL << R_IN2) |
                        (1ULL << L_IN1) |
                        (1ULL << L_IN2) |
                        (1ULL << TRIG),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_out));

    gpio_set_level(R_IN1, 0);
    gpio_set_level(R_IN2, 0);
    gpio_set_level(L_IN1, 0);
    gpio_set_level(L_IN2, 0);
    gpio_set_level(TRIG, 0);

    gpio_config_t io_tcrt = {
        .pin_bit_mask = (1ULL << TCRT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_tcrt));

    gpio_config_t io_echo = {
        .pin_bit_mask = (1ULL << ECHO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io_echo));

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(ECHO, echo_isr_handler, NULL));

    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ch_dir = {
        .gpio_num   = R_PWM,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CH_DIR,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_dir));

    ledc_channel_config_t ch_esq = {
        .gpio_num   = L_PWM,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CH_ESQ,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_esq));
}

// =========================================================================
// 6. CONTROLE DOS MOTORES (TB6612FNG - COM SHORT BRAKE)
// =========================================================================
static void controlar_motor_direito(int velocidade) {
    if (INVERTER_MOTOR_DIR) velocidade = -velocidade;
    velocidade = limitar(velocidade, -255, 255);

    if (velocidade > 0) {
        gpio_set_level(R_IN1, 1);
        gpio_set_level(R_IN2, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CH_DIR, (uint32_t)velocidade);
    } else if (velocidade < 0) {
        gpio_set_level(R_IN1, 0);
        gpio_set_level(R_IN2, 1);
        ledc_set_duty(LEDC_MODE, LEDC_CH_DIR, (uint32_t)(-velocidade));
    } else {
        gpio_set_level(R_IN1, 0);
        gpio_set_level(R_IN2, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CH_DIR, 0);
    }
    ledc_update_duty(LEDC_MODE, LEDC_CH_DIR);
}

static void controlar_motor_esquerdo(int velocidade) {
    if (INVERTER_MOTOR_ESQ) velocidade = -velocidade;
    velocidade = limitar(velocidade, -255, 255);

    if (velocidade > 0) {
        gpio_set_level(L_IN1, 1);
        gpio_set_level(L_IN2, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ, (uint32_t)velocidade);
    } else if (velocidade < 0) {
        gpio_set_level(L_IN1, 0);
        gpio_set_level(L_IN2, 1);
        ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ, (uint32_t)(-velocidade));
    } else {
        gpio_set_level(L_IN1, 0);
        gpio_set_level(L_IN2, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ, 0);
    }
    ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ);
}

static void mover(int vel_esq, int vel_dir) {
    controlar_motor_esquerdo(vel_esq);
    controlar_motor_direito(vel_dir);
}

static void parar_motores(void) {
    mover(0, 0);
}

static void freiar_motores(void) {
    gpio_set_level(R_IN1, 1);
    gpio_set_level(R_IN2, 1);
    ledc_set_duty(LEDC_MODE, LEDC_CH_DIR, 255);
    ledc_update_duty(LEDC_MODE, LEDC_CH_DIR);

    gpio_set_level(L_IN1, 1);
    gpio_set_level(L_IN2, 1);
    ledc_set_duty(LEDC_MODE, LEDC_CH_ESQ, 255);
    ledc_update_duty(LEDC_MODE, LEDC_CH_ESQ);
}

// =========================================================================
// 7. SENSORES (TCRT5000 COM DEBOUNCE + HC-SR04 COM DESTRAVAMENTO)
// =========================================================================
static bool detectou_borda(void) {
    if (gpio_get_level(TCRT) == NIVEL_BORDA_BRANCA) {
        if (contador_borda < DEBOUNCE_BORDA_AMOSTRAS) {
            contador_borda++;
        }
    } else {
        contador_borda = 0;
    }
    return contador_borda >= DEBOUNCE_BORDA_AMOSTRAS;
}

static void disparar_ultrassom(void) {
    portENTER_CRITICAL(&echo_mux);
    echo_inicio_us = 0;
    echo_pronto = false;
    portEXIT_CRITICAL(&echo_mux);

    gpio_set_level(TRIG, 0);
    esp_rom_delay_us(4);
    gpio_set_level(TRIG, 1);
    esp_rom_delay_us(12);
    gpio_set_level(TRIG, 0);
}

static void atualizar_ultrassom(uint32_t agora) {
    bool amostra_pronta = false;
    int64_t duracao = 0;

    portENTER_CRITICAL(&echo_mux);
    if (echo_pronto) {
        duracao = echo_duracao_us;
        echo_pronto = false;
        amostra_pronta = true;
    }
    portEXIT_CRITICAL(&echo_mux);

    if (amostra_pronta) {
        aguardando_echo = false;
        if (duracao >= ECHO_MIN_US && duracao <= ECHO_MAX_US) {
            distancia_atual = (duracao * 0.0343f) / 2.0f;
        } else {
            distancia_atual = 999.0f;
        }

        if (ping_atual_ms >= tempo_inicio_estado) {
            leitura_nova_no_estado = true;
        }
    } else if (aguardando_echo && (agora - ping_atual_ms) >= TIMEOUT_ECHO_MS) {
        aguardando_echo = false;
        distancia_atual = 999.0f;

        if (ping_atual_ms >= tempo_inicio_estado) {
            leitura_nova_no_estado = true;
        }
    }

    if ((agora - ultimo_ping_ms) >= INTERVALO_ULTRASSOM_MS) {
        bool echo_livre = (gpio_get_level(ECHO) == 0);
        bool echo_travado = ((agora - ping_atual_ms) >= TIMEOUT_ECHO_TRAVADO_MS);

        if (echo_livre || echo_travado) {
            ultimo_ping_ms = agora;
            ping_atual_ms = agora;
            disparar_ultrassom();
            aguardando_echo = true;
        }
    }
}

static const char *nome_estado(estado_robo_t st) {
    switch (st) {
        case BUSCA_GIRAR:      return "BUSCA_GIRAR";
        case BUSCA_AVANCAR:    return "BUSCA_AVANCAR";
        case FREIO_ALVO:       return "FREIO_ALVO";
        case CORRECAO_ALVO:    return "CORRECAO_ALVO";
        case ATACAR:           return "ATACAR";
        case FUGA_BORDA_FREIO: return "FUGA_BORDA_FREIO";
        case FUGA_BORDA_RE:    return "FUGA_BORDA_RE";
        case FUGA_BORDA_GIRO:  return "FUGA_BORDA_GIRO";
        default:               return "DESCONHECIDO";
    }
}

// =========================================================================
// 8. PONTO DE ENTRADA (app_main)
// =========================================================================
void app_main(void) {
    configurar_hardware();
    parar_motores();

    ESP_LOGI(TAG, "=== ROBO SUMO ESP-IDF INICIADO (POTENCIA %.0f%%) ===",
             FATOR_POTENCIA * 100.0f);
    ESP_LOGI(TAG, "Aguardando 5 segundos de seguranca...");

    uint32_t inicio_espera = millis_agora();
    vTaskDelay(pdMS_TO_TICKS(TEMPO_ESPERA_INICIAL_MS - TEMPO_PRE_SCAN_MS));

    // Pré-carrega leituras do ultrassom nos últimos 400 ms com motores parados
    while ((millis_agora() - inicio_espera) < TEMPO_ESPERA_INICIAL_MS) {
        atualizar_ultrassom(millis_agora());
        vTaskDelay(LOOP_DELAY_TICKS);
    }

    uint32_t tempo_ultimo_alvo_ms = millis_agora();
    bool alvo_muito_perto = false;

    if (distancia_atual <= DISTANCIA_ATAQUE_CM) {
        ESP_LOGI(TAG, "COMBATE INICIADO! Alvo a frente (%.1f cm) -> ATACAR", distancia_atual);
        alvo_muito_perto = (distancia_atual <= DISTANCIA_PONTO_CEGO_CM);
        mudar_estado(ATACAR);
    } else {
        ESP_LOGI(TAG, "COMBATE INICIADO! Iniciando varredura no eixo.");
        mudar_estado(BUSCA_GIRAR);
    }

    uint32_t ultimo_debug = 0;

    while (1) {
        uint32_t agora = millis_agora();
        bool borda = detectou_borda();
        atualizar_ultrassom(agora);

        bool alvo_visivel = (distancia_atual <= DISTANCIA_ATAQUE_CM);
        if (alvo_visivel) {
            tempo_ultimo_alvo_ms = agora;
            alvo_muito_perto = (distancia_atual <= DISTANCIA_PONTO_CEGO_CM);
        }

        // -----------------------------------------------------------------
        // PRIORIDADE 1: FUGA DA BORDA BRANCA (freia -> ré curta -> gira)
        // -----------------------------------------------------------------
        bool em_fuga = (estado_atual == FUGA_BORDA_FREIO ||
                        estado_atual == FUGA_BORDA_RE ||
                        estado_atual == FUGA_BORDA_GIRO);

        if (borda && !em_fuga) {
            alvo_muito_perto = false;
            mudar_estado(FUGA_BORDA_FREIO);
            freiar_motores();
        }

        if (estado_atual == FUGA_BORDA_FREIO) {
            freiar_motores();
            if ((agora - tempo_inicio_estado) >= TEMPO_FREIO_BORDA_MS) {
                mudar_estado(FUGA_BORDA_RE);
            }
        }
        else if (estado_atual == FUGA_BORDA_RE) {
            int vel_re = ((agora - tempo_inicio_estado) < TEMPO_RAMPA_RE_MS)
                         ? VEL_RE_INICIAL : VEL_RE_BORDA;
            mover(-vel_re, -vel_re);

            if ((agora - tempo_inicio_estado) >= TEMPO_RE_BORDA_MS) {
                mudar_estado(FUGA_BORDA_GIRO);
            }
        }
        else if (estado_atual == FUGA_BORDA_GIRO) {
            if (girar_para_esquerda) {
                mover(-VEL_GIRO, VEL_GIRO);
            } else {
                mover(VEL_GIRO, -VEL_GIRO);
            }

            if ((agora - tempo_inicio_estado) >= TEMPO_GIRO_BORDA_MS) {
                girar_para_esquerda = !girar_para_esquerda;
                if (borda) {
                    mudar_estado(FUGA_BORDA_RE);
                } else {
                    mudar_estado(BUSCA_GIRAR);
                }
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 2: FREIO DE TRAVAMENTO COM LEITURA CONFIRMADA
        // -----------------------------------------------------------------
        else if (estado_atual == FREIO_ALVO) {
            freiar_motores();
            uint32_t tempo_freio = agora - tempo_inicio_estado;
            bool pronto_decidir = (tempo_freio >= TEMPO_FREIO_ALVO_MS && leitura_nova_no_estado) ||
                                  (tempo_freio >= TEMPO_FREIO_ALVO_MAX_MS);

            if (pronto_decidir) {
                if (alvo_visivel) {
                    mudar_estado(ATACAR);
                } else {
                    mudar_estado(CORRECAO_ALVO);
                }
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 3: ATAQUE CONTÍNUO COM INÉRCIA DE IMPACTO
        // -----------------------------------------------------------------
        else if (alvo_visivel) {
            bool estava_em_giro_rapido = (estado_atual == BUSCA_GIRAR &&
                                          (agora - tempo_inicio_estado) >= TEMPO_MIN_GIRO_FREIO_MS);

            if (estava_em_giro_rapido) {
                mudar_estado(FREIO_ALVO);
                freiar_motores();
            } else {
                if (estado_atual != ATACAR) {
                    mudar_estado(ATACAR);
                }
                int vel_atk = ((agora - tempo_inicio_estado) < TEMPO_RAMPA_ATAQUE_MS)
                              ? VEL_ATAQUE_INICIAL : VEL_ATAQUE;
                mover(vel_atk, vel_atk);
            }
        }
        else if (estado_atual == ATACAR) {
            uint32_t tempo_sem_alvo = agora - tempo_ultimo_alvo_ms;
            uint32_t tolerancia = alvo_muito_perto
                                  ? TEMPO_INERCIA_IMPACTO_MS
                                  : TEMPO_TOLERANCIA_FALHA_MS;

            if (tempo_sem_alvo < tolerancia) {
                int vel_atk = ((agora - tempo_inicio_estado) < TEMPO_RAMPA_ATAQUE_MS)
                              ? VEL_ATAQUE_INICIAL : VEL_ATAQUE;
                mover(vel_atk, vel_atk);
            } else {
                alvo_muito_perto = false;
                mudar_estado(BUSCA_GIRAR);
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 4: CONTRAGIRO DE CORREÇÃO E BUSCA NO EIXO
        // -----------------------------------------------------------------
        else if (estado_atual == CORRECAO_ALVO) {
            if (girar_para_esquerda) {
                mover(VEL_CORRECAO, -VEL_CORRECAO);
            } else {
                mover(-VEL_CORRECAO, VEL_CORRECAO);
            }

            if ((agora - tempo_inicio_estado) >= TEMPO_CORRECAO_ALVO_MS) {
                girar_para_esquerda = !girar_para_esquerda;
                mudar_estado(BUSCA_GIRAR);
            }
        }
        else if (estado_atual == BUSCA_GIRAR) {
            if (girar_para_esquerda) {
                mover(-VEL_GIRO, VEL_GIRO);
            } else {
                mover(VEL_GIRO, -VEL_GIRO);
            }

            if ((agora - tempo_inicio_estado) >= TEMPO_GIRO_BUSCA_MS) {
                girar_para_esquerda = !girar_para_esquerda;
                mudar_estado(BUSCA_AVANCAR);
            }
        }
        else if (estado_atual == BUSCA_AVANCAR) {
            mover(VEL_BUSCA, VEL_BUSCA);
            if ((agora - tempo_inicio_estado) >= TEMPO_AVANCO_BUSCA_MS) {
                mudar_estado(BUSCA_GIRAR);
            }
        }

        // -----------------------------------------------------------------
        // DEBUG NO MONITOR SERIAL (a cada 200 ms)
        // -----------------------------------------------------------------
        if ((agora - ultimo_debug) >= 200) {
            ultimo_debug = agora;
            ESP_LOGI(TAG, "Estado: %-16s | Dist: %5.1f cm | Borda: %s | Perto: %s",
                     nome_estado(estado_atual),
                     distancia_atual,
                     borda ? "SIM" : "NAO",
                     alvo_muito_perto ? "SIM" : "NAO");
        }

        vTaskDelay(LOOP_DELAY_TICKS);
    }
}