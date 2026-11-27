/*
 * Diagnóstico do HC-SR04 (ESP-IDF). Motores ficam DESLIGADOS o tempo todo.
 * A cada 200 ms mostra o sinal cru do ECHO:
 *   - ECHO antes: nível do pino ANTES do disparo (o normal é 0)
 *   - ocupado:    se estava em 1, quanto tempo levou para voltar a 0 (eco antigo)
 *   - subiu apos: tempo entre o disparo e o ECHO subir (o normal é algumas centenas de us)
 *   - pulso:      largura do ECHO em us (distância = pulso / 58)
 * -1 = não aconteceu dentro do limite de espera.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "pins.h"

// Espera o ECHO chegar ao nível pedido; devolve o tempo em us ou -1 se passar do limite.
static long long esperar_echo(int nivel, long long limite_us) {
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(ECHO) != nivel) {
        if (esp_timer_get_time() - t0 > limite_us) return -1;
    }
    return esp_timer_get_time() - t0;
}

void app_main(void) {
    gpio_config_t saidas = {
        .pin_bit_mask = (1ULL << R_IN1) | (1ULL << R_IN2) | (1ULL << R_PWM) |
                        (1ULL << L_IN1) | (1ULL << L_IN2) | (1ULL << L_PWM) | (1ULL << TRIG),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&saidas));
    gpio_num_t pinos[] = {R_IN1, R_IN2, R_PWM, L_IN1, L_IN2, L_PWM, TRIG};
    for (int i = 0; i < 7; i++) gpio_set_level(pinos[i], 0);  // motores parados

    gpio_config_t entrada = {
        .pin_bit_mask = 1ULL << ECHO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&entrada));

    printf("\n=== DIAGNOSTICO HC-SR04 (motores desligados) ===\n");
    while (1) {
        int antes = gpio_get_level(ECHO);
        long long ocupado = antes ? esperar_echo(0, 250000) : 0;

        gpio_set_level(TRIG, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG, 0);

        long long subida = esperar_echo(1, 30000);
        long long pulso = subida < 0 ? -1 : esperar_echo(0, 250000);

        printf("ECHO antes: %d | ocupado: %lld us | subiu apos: %lld us | pulso: %lld us | distancia: %.1f cm\n",
               antes, ocupado, subida, pulso, pulso > 0 ? pulso / 58.0 : -1.0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
