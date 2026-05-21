/**
 * @file    sensor_task.cpp
 * @brief   Sensor Task — reads hardware sensors, validates data,
 *          sends packets to the shared queue, and reports heartbeat.
 *
 * FAULT SIMULATION
 * ────────────────
 *  g_faultInject.freeze_sensor  → task enters infinite delay (no heartbeat)
 *  g_faultInject.corrupt_sensor → sends packets with data_valid = false
 *                                  and out-of-range temperature values
 *
 * NORMAL OPERATION
 * ────────────────
 *  1. Read NTC thermistor via ADC (GPIO 34)
 *  2. Convert raw ADC → Celsius using Steinhart-Hart approximation
 *  3. Build SensorData_t packet
 *  4. Push to g_sensorDataQueue (non-blocking; drop if full)
 *  5. Update heartbeat
 *  6. Delay 1 second
 */

#include "sensor_task.h"
#include "../config/system_config.h"
#include <math.h>   // log()

// ─── NTC Thermistor Constants (10kΩ NTC, β=3950) ────────────────────────────

#define NTC_SERIES_R        10000.0f   ///< Series resistor (Ω)
#define NTC_NOMINAL_R       10000.0f   ///< Resistance at 25°C (Ω)
#define NTC_NOMINAL_TEMP_C  25.0f      ///< Reference temperature
#define NTC_BETA            3950.0f    ///< Beta coefficient
#define ADC_MAX             4095.0f    ///< 12-bit ESP32 ADC

// ─── Private: ADC → Celsius ───────────────────────────────────────────────────

/**
 * @brief Convert raw 12-bit ADC reading to Celsius.
 *        Uses simplified Steinhart-Hart (β-parameter equation).
 */
static float adcToCelsius(uint16_t raw)
{
    if (raw == 0) return -99.0f; // Short circuit guard

    float resistance = NTC_SERIES_R * ((ADC_MAX / (float)raw) - 1.0f);
    float steinhart  = resistance / NTC_NOMINAL_R;           // R/R0
    steinhart        = logf(steinhart);                       // ln(R/R0)
    steinhart       /= NTC_BETA;                              // / Beta
    steinhart       += 1.0f / (NTC_NOMINAL_TEMP_C + 273.15f);// + 1/T0
    return (1.0f / steinhart) - 273.15f;                      // → Celsius
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

void sensorTask(void* pvParameters)
{
    GB_LOG(TASK_SENSOR, LOG_INFO, "SensorTask online");

    // Configure ADC pin
    pinMode(PIN_TEMP_SENSOR, INPUT);

    for (;;) {
        // ── FAULT INJECTION: freeze simulation ───────────────────────────
        if (g_faultInject.freeze_sensor) {
            GB_LOG(TASK_SENSOR, LOG_WARNING,
                   "[FAULT SIM] SensorTask frozen — heartbeat suspended");
            // Deliberately stall; HealthMonitor will detect heartbeat miss
            while (g_faultInject.freeze_sensor) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            GB_LOG(TASK_SENSOR, LOG_INFO, "[FAULT SIM] freeze cleared");
        }

        // ── Read sensor ───────────────────────────────────────────────────
        uint16_t raw  = (uint16_t)analogRead(PIN_TEMP_SENSOR);
        float    temp = adcToCelsius(raw);

        // ── Build data packet ─────────────────────────────────────────────
        SensorData_t pkt;
        pkt.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        pkt.sensor_id    = 0;

        if (g_faultInject.corrupt_sensor) {
            // FAULT INJECTION: bad data
            pkt.temperature = 999.9f;
            pkt.data_valid  = false;
            GB_LOG(TASK_SENSOR, LOG_WARNING,
                   "[FAULT SIM] injecting corrupted sensor packet");
        } else {
            pkt.temperature = temp;
            pkt.data_valid  = (temp > SENSOR_TEMP_MIN_C &&
                               temp < SENSOR_TEMP_MAX_C);
        }

        // ── Push to queue (non-blocking; log if full) ─────────────────────
        if (xQueueSend(g_sensorDataQueue, &pkt, 0) != pdTRUE) {
            GB_LOG(TASK_SENSOR, LOG_WARNING, "Sensor queue full — packet dropped");
        }

        // ── Heartbeat ─────────────────────────────────────────────────────
        HEARTBEAT(TASK_SENSOR);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
