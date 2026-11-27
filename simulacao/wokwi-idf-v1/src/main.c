/*
 * Robô Sumô Autônomo - ESP32 (ESP-IDF / C puro)
 * Pinagem definida em src/pins.h
 * Correções: Short Brake (TB6612FNG), freio de travamento de alvo e loop rápido de borda
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
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
// 2. CONFIGURAÇÕES DE CALIBRAÇÃO (AJUSTE AQUI NA BANCADA)
// =========================================================================
// Padrão TCRT5000 (LM393): 0 = linha branca, 1 = arena preta.
// Se no teste o sensor acionar invertido, troque 0 para 1:
#define NIVEL_BORDA_BRANCA     0

// Se alguma roda girar ao contrário no avanço, troque false para true:
#define INVERTER_MOTOR_DIR     false
#define INVERTER_MOTOR_ESQ     false

// Distância máxima para travar e atacar (em cm)
#define DISTANCIA_ATAQUE_CM    40.0f

// Espera regulamentar após ligar a chave (5 segundos)
#define TEMPO_ESPERA_INICIAL   5000

// Velocidades PWM (0 a 255) - reduzidas na busca/giro para evitar overshoot
#define VEL_ATAQUE             220   // Forte, mas ainda permite frear na borda (máx: 255)
#define VEL_BUSCA              125   // Avanço controlado na arena
#define VEL_GIRO               115   // Giro firme para o HC-SR04 ler sem passar direto
#define VEL_RE_BORDA           190   // Ré rápida para sair da linha branca

// Tempos de frenagem e manobras (em ms)
#define TEMPO_FREIO_ALVO       70    // Freio eletromagnético ao sair do giro pro ataque
#define TEMPO_FREIO_BORDA      50    // Freio imediato ao tocar a linha branca antes da ré
#define TEMPO_AVANCO_BUSCA     600   // Tempo avançando reto na busca
#define TEMPO_GIRO_270         1050  // Calibrado para a nova VEL_GIRO menor
#define TEMPO_RE_BORDA         350   // Tempo de ré após frear na borda
#define TEMPO_GIRO_BORDA       550   // Tempo girando ~150° a 180° para voltar ao centro

// Controle de leitura do HC-SR04 (3500 us ≈ 60 cm máx; loop não trava sem alvo)
#define TIMEOUT_ULTRASSOM_US   3500
#define INTERVALO_ULTRASSOM_MS 25    // Evita eco fantasma mantendo ~40 leituras/seg

// Garante pelo menos 1 tick do FreeRTOS independente do CONFIG_FREERTOS_HZ
#define LOOP_DELAY_TICKS       (pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1)

// =========================================================================
// 3. MÁQUINA DE ESTADOS
// =========================================================================
typedef enum {
    BUSCA_AVANCAR,
    BUSCA_GIRAR,
    FREIO_ALVO,
    ATACAR,
    FUGA_BORDA_FREIO,
    FUGA_BORDA_RE,
    FUGA_BORDA_GIRO
} estado_robo_t;

static estado_robo_t estado_atual = BUSCA_AVANCAR;
static uint32_t tempo_inicio_estado = 0;
static bool girar_para_esquerda = true;

// =========================================================================
// 4. FUNÇÕES AUXILIARES E HARDWARE
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

    gpio_config_t io_in = {
        .pin_bit_mask = (1ULL << ECHO) | (1ULL << TCRT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_in));

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
// 5. CONTROLE DOS MOTORES (TB6612FNG - COM SHORT BRAKE REAL)
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
        // Ponto morto / roda livre (High-Z)
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

// Freio eletromagnético (Short Brake da TB6612FNG: IN1=1, IN2=1)
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
// 6. LEITURA DOS SENSORES
// =========================================================================
static bool detectou_borda(void) {
    return gpio_get_level(TCRT) == NIVEL_BORDA_BRANCA;
}

static float ler_distancia_cm(void) {
    gpio_set_level(TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level(TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIG, 0);

    int64_t espera_inicio = esp_timer_get_time();
    while (gpio_get_level(ECHO) == 0) {
        if ((esp_timer_get_time() - espera_inicio) > 2000) {
            return 999.0f;
        }
    }

    int64_t pulso_inicio = esp_timer_get_time();
    while (gpio_get_level(ECHO) == 1) {
        if ((esp_timer_get_time() - pulso_inicio) > TIMEOUT_ULTRASSOM_US) {
            return 999.0f;
        }
    }

    int64_t duracao_us = esp_timer_get_time() - pulso_inicio;
    return (duracao_us * 0.0343f) / 2.0f;
}

static const char *nome_estado(estado_robo_t st) {
    switch (st) {
        case BUSCA_AVANCAR:    return "BUSCA_AVANCAR";
        case BUSCA_GIRAR:      return "BUSCA_GIRAR";
        case FREIO_ALVO:       return "FREIO_ALVO";
        case ATACAR:           return "ATACAR";
        case FUGA_BORDA_FREIO: return "FUGA_BORDA_FREIO";
        case FUGA_BORDA_RE:    return "FUGA_BORDA_RE";
        case FUGA_BORDA_GIRO:  return "FUGA_BORDA_GIRO";
        default:               return "DESCONHECIDO";
    }
}

// =========================================================================
// 7. PONTO DE ENTRADA (app_main)
// =========================================================================
void app_main(void) {
    configurar_hardware();
    parar_motores();

    ESP_LOGI(TAG, "=== ROBO SUMO ESP-IDF INICIADO ===");
    ESP_LOGI(TAG, "Aguardando 5 segundos de seguranca...");
    vTaskDelay(pdMS_TO_TICKS(TEMPO_ESPERA_INICIAL));

    ESP_LOGI(TAG, "COMBATE INICIADO!");
    mudar_estado(BUSCA_AVANCAR);

    uint32_t ultimo_debug = 0;
    uint32_t ultima_leitura_us = 0;
    float distancia = 999.0f;

    while (1) {
        uint32_t agora = millis_agora();

        // Borda é verificada em TODA iteração (~10 ms) para reação instantânea
        bool borda = detectou_borda();

        // Ultrassom atualizado em intervalo fixo para evitar ecos fantasmas
        if ((agora - ultima_leitura_us) >= INTERVALO_ULTRASSOM_MS) {
            ultima_leitura_us = agora;
            distancia = ler_distancia_cm();
        }

        // -----------------------------------------------------------------
        // PRIORIDADE 1: FUGA DA BORDA BRANCA (freia -> ré -> gira)
        // -----------------------------------------------------------------
        bool em_fuga = (estado_atual == FUGA_BORDA_FREIO ||
                        estado_atual == FUGA_BORDA_RE ||
                        estado_atual == FUGA_BORDA_GIRO);

        if (borda && !em_fuga) {
            mudar_estado(FUGA_BORDA_FREIO);
            freiar_motores();
        }

        if (estado_atual == FUGA_BORDA_FREIO) {
            freiar_motores();
            if ((agora - tempo_inicio_estado) >= TEMPO_FREIO_BORDA) {
                mudar_estado(FUGA_BORDA_RE);
            }
        }
        else if (estado_atual == FUGA_BORDA_RE) {
            mover(-VEL_RE_BORDA, -VEL_RE_BORDA);
            if ((agora - tempo_inicio_estado) >= TEMPO_RE_BORDA) {
                mudar_estado(FUGA_BORDA_GIRO);
            }
        }
        else if (estado_atual == FUGA_BORDA_GIRO) {
            if (girar_para_esquerda) {
                mover(-VEL_GIRO, VEL_GIRO);
            } else {
                mover(VEL_GIRO, -VEL_GIRO);
            }

            if ((agora - tempo_inicio_estado) >= TEMPO_GIRO_BORDA) {
                girar_para_esquerda = !girar_para_esquerda;
                mudar_estado(BUSCA_AVANCAR);
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 2: FREIO DE TRAVAMENTO DO ALVO (mata inércia do giro)
        // -----------------------------------------------------------------
        else if (estado_atual == FREIO_ALVO) {
            freiar_motores();
            if ((agora - tempo_inicio_estado) >= TEMPO_FREIO_ALVO) {
                if (distancia <= DISTANCIA_ATAQUE_CM) {
                    mudar_estado(ATACAR);
                } else {
                    // Se ainda passou um pouco do alvo, inverte o giro para voltar nele
                    girar_para_esquerda = !girar_para_esquerda;
                    mudar_estado(BUSCA_GIRAR);
                }
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 3: ATAQUE
        // -----------------------------------------------------------------
        else if (distancia <= DISTANCIA_ATAQUE_CM) {
            if (estado_atual == BUSCA_GIRAR) {
                // Estava girando: trava as rodas primeiro antes de disparar reto
                mudar_estado(FREIO_ALVO);
                freiar_motores();
            } else {
                if (estado_atual != ATACAR) {
                    mudar_estado(ATACAR);
                }
                mover(VEL_ATAQUE, VEL_ATAQUE);
            }
        }
        // -----------------------------------------------------------------
        // PRIORIDADE 4: BUSCA (avança -> gira 270° -> avança nova direção)
        // -----------------------------------------------------------------
        else {
            if (estado_atual == ATACAR) {
                mudar_estado(BUSCA_GIRAR);
            }

            if (estado_atual == BUSCA_AVANCAR) {
                mover(VEL_BUSCA, VEL_BUSCA);
                if ((agora - tempo_inicio_estado) >= TEMPO_AVANCO_BUSCA) {
                    mudar_estado(BUSCA_GIRAR);
                }
            }
            else if (estado_atual == BUSCA_GIRAR) {
                if (girar_para_esquerda) {
                    mover(-VEL_GIRO, VEL_GIRO);
                } else {
                    mover(VEL_GIRO, -VEL_GIRO);
                }

                if ((agora - tempo_inicio_estado) >= TEMPO_GIRO_270) {
                    girar_para_esquerda = !girar_para_esquerda;
                    mudar_estado(BUSCA_AVANCAR);
                }
            }
        }

        // -----------------------------------------------------------------
        // DEBUG NO MONITOR SERIAL (a cada 200 ms)
        // -----------------------------------------------------------------
        if ((agora - ultimo_debug) >= 200) {
            ultimo_debug = agora;
            ESP_LOGI(TAG, "Estado: %-16s | Dist: %5.1f cm | Borda: %s",
                     nome_estado(estado_atual),
                     distancia,
                     borda ? "SIM" : "NAO");
        }

        vTaskDelay(LOOP_DELAY_TICKS);
    }
}
