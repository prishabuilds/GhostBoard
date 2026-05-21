/**
 * @file    comm_task.cpp
 * @brief   Communication Task — simulates UART/WiFi/MQTT telemetry,
 *          handles retries, detects timeouts, and reports heartbeat.
 *
 * FAULT SIMULATION
 * ────────────────
 *  g_faultInject.comm_timeout → stalls send loop, no heartbeat
 *
 * NORMAL OPERATION
 * ────────────────
 *  1. Pull latest sensor data from queue (peek, don't consume)
 *  2. Serialize to JSON-like string
 *  3. "Transmit" over Serial2 (or stub for simulation)
 *  4. Handle retries up to COMM_MAX_RETRIES
 *  5. Log transmission result
 *  6. Update heartbeat every cycle
 */

#include "comm_task.h"
#include "../config/system_config.h"

#define COMM_MAX_RETRIES     3
#define COMM_RETRY_DELAY_MS  500
#define COMM_TX_INTERVAL_MS  2000  ///< Transmit every 2 seconds

// ─── Private: Format & transmit ──────────────────────────────────────────────

/**
 * @brief Serialize a SensorData_t to a compact JSON string and
 *        write it to Serial (simulates UART/WiFi transmission).
 *
 * @return true  on simulated success
 * @return false on simulated failure (random 5% drop rate)
 */
static bool transmitPacket(const SensorData_t* data)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ts\":%lu,\"temp\":%.2f,\"valid\":%s,\"state\":%d}",
             (unsigned long)data->timestamp_ms,
             data->temperature,
             data->data_valid ? "true" : "false",
             (int)g_systemState);

    Serial.println(buf);

    // Simulate 5% random packet loss to make the system interesting
    return (esp_random() % 100) >= 5;
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

void commTask(void* pvParameters)
{
    GB_LOG(TASK_COMM, LOG_INFO, "CommTask online");

    for (;;) {
        // ── FAULT INJECTION: communication timeout ────────────────────────
        if (g_faultInject.comm_timeout) {
            GB_LOG(TASK_COMM, LOG_WARNING,
                   "[FAULT SIM] CommTask stalled — no heartbeat");
            while (g_faultInject.comm_timeout) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            GB_LOG(TASK_COMM, LOG_INFO, "[FAULT SIM] comm timeout cleared");
        }

        // ── Peek at latest sensor data ────────────────────────────────────
        SensorData_t data;
        if (xQueuePeek(g_sensorDataQueue, &data, pdMS_TO_TICKS(100)) != pdTRUE) {
            // No data yet — log and wait
            GB_LOG(TASK_COMM, LOG_DEBUG, "No sensor data available");
            HEARTBEAT(TASK_COMM);
            vTaskDelay(pdMS_TO_TICKS(COMM_TX_INTERVAL_MS));
            continue;
        }

        // ── Transmit with retry logic ─────────────────────────────────────
        bool sent = false;
        for (int attempt = 1; attempt <= COMM_MAX_RETRIES; attempt++) {
            sent = transmitPacket(&data);
            if (sent) {
                if (attempt > 1) {
                    GB_LOG(TASK_COMM, LOG_INFO,
                           "Packet sent on attempt %d", attempt);
                }
                break;
            }
            GB_LOG(TASK_COMM, LOG_WARNING,
                   "TX failed (attempt %d/%d) — retrying",
                   attempt, COMM_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(COMM_RETRY_DELAY_MS));
        }

        if (!sent) {
            GB_LOG(TASK_COMM, LOG_ERROR,
                   "All %d TX attempts failed — packet lost", COMM_MAX_RETRIES);
        }

        // ── Heartbeat ─────────────────────────────────────────────────────
        HEARTBEAT(TASK_COMM);

        vTaskDelay(pdMS_TO_TICKS(COMM_TX_INTERVAL_MS));
    }
}
