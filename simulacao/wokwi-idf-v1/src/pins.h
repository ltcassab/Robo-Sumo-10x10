#pragma once

// gpio_num_t
#include "hal/gpio_types.h"

// HC-SR04 (ultrassom)
#define TRIG   GPIO_NUM_22
#define ECHO   GPIO_NUM_35  // Input-only (via conversor lv1 / hv1)

// Ponte H TB6612FNG - Motor A (Direita)
#define R_IN1  GPIO_NUM_14  // AIN1
#define R_IN2  GPIO_NUM_12  // AIN2 (strapping pin com pull-down na TB6612FNG)
#define R_PWM  GPIO_NUM_13  // PWMA

// Ponte H TB6612FNG - Motor B (Esquerda)
#define L_IN1  GPIO_NUM_26  // BIN1
#define L_IN2  GPIO_NUM_33  // BIN2
#define L_PWM  GPIO_NUM_16  // PWMB

// Sensor de borda TCRT5000 (pino D0 via conversor lv4 / hv4)
#define TCRT   GPIO_NUM_34  // Input-only
