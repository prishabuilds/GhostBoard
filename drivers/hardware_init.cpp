/**
 * @file    hardware_init.cpp
 * @brief   One-time hardware peripheral initialization called from setup().
 *          Configures GPIO, ADC, SPI, and software watchdog.
 */

#include "hardware_init.h"
#include "../config/system_config.h"
#include "esp_task_wdt.h"
#include "SPI.h"

/**
 * @brief Initialize all GhostBoard hardware peripherals.
 * @return true on success, false if a critical peripheral fails.
 */
bool hardware_init(void)
{
    // ── Status LEDs ───────────────────────────────────────────────────────
    pinMode(PIN_LED_HEALTHY,  OUTPUT);
    pinMode(PIN_LED_WARNING,  OUTPUT);
    pinMode(PIN_LED_CRITICAL, OUTPUT);
    digitalWrite(PIN_LED_HEALTHY,  LOW);
    digitalWrite(PIN_LED_WARNING,  LOW);
    digitalWrite(PIN_LED_CRITICAL, LOW);

    // Boot blink to confirm LEDs work
    digitalWrite(PIN_LED_HEALTHY, HIGH);
    delay(200);
    digitalWrite(PIN_LED_WARNING, HIGH);
    delay(200);
    digitalWrite(PIN_LED_CRITICAL, HIGH);
    delay(200);
    digitalWrite(PIN_LED_HEALTHY,  LOW);
    digitalWrite(PIN_LED_WARNING,  LOW);
    digitalWrite(PIN_LED_CRITICAL, LOW);

    // ── Buzzer ────────────────────────────────────────────────────────────
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);

    // ── ADC (sensor pin is input by default — explicit for clarity) ───────
    pinMode(PIN_TEMP_SENSOR, INPUT);
    analogReadResolution(12); // 12-bit ADC on ESP32

    // ── SPI for SD card ───────────────────────────────────────────────────
    SPI.begin();
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // ── ESP32 Hardware Watchdog Timer ────────────────────────────────────
    // 10 s timeout — HealthMonitor feeds it every 500 ms
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = 10000,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_task_wdt_init(&wdt_cfg);

    Serial.println("[HW] Peripherals initialized");
    return true;
}

/**
 * @brief Drive status LEDs and buzzer based on current system state.
 *        Call periodically from a low-priority task or from HealthMonitor.
 */
void hardware_updateStatus(SystemState_t state)
{
    digitalWrite(PIN_LED_HEALTHY,  state == STATE_HEALTHY    ? HIGH : LOW);
    digitalWrite(PIN_LED_WARNING,  state == STATE_NERVOUS    ||
                                   state == STATE_RECOVERING ? HIGH : LOW);
    digitalWrite(PIN_LED_CRITICAL, state == STATE_CRITICAL   ? HIGH : LOW);

    // Buzzer: short beep on CRITICAL
    if (state == STATE_CRITICAL) {
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(PIN_BUZZER, LOW);
    }
}
