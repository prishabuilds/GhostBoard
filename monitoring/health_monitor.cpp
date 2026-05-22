/**
 * @file    health_monitor.cpp
 * @brief   Health Monitor Task — the central nervous system of GhostBoard.
 *
 * RESPONSIBILITIES
 * ────────────────
 *  • Poll heartbeat timestamps for every registered task.
 *  • Detect task freezes, timing violations, and memory overuse.
 *  • Validate incoming sensor data for out-of-range readings.
 *  • Maintain a fault history and escalate the system state when
 *    failures accumulate.
 *  • Issue recovery commands to RecoveryManager via g_recoveryQueue.
 *  • Feed the software watchdog; if this task itself freezes, the
 *    hardware watchdog fires and forces a full reset.
 *
 * STATE MACHINE
 * ─────────────
 *  HEALTHY ──(miss)──► NERVOUS ──(escalate)──► CRITICAL
 *     ▲                                             │
 *     └──────── RECOVERING ◄──(cmd issued)──────────┘
 */

#include "health_monitor.h"
#include "../config/system_config.h"
#include "esp_task_wdt.h"   // ESP32 hardware watchdog API

// ─── Private Types ────────────────────────────────────────────────────────────

/** Per-task tracking record maintained by the Health Monitor */
typedef struct {
    const char* name;
    bool        registered;   ///< Task was ever seen alive
    bool        healthy;
    uint8_t     miss_count;   ///< Consecutive heartbeat misses
    TickType_t  last_seen;    ///< Last good heartbeat tick
    UBaseType_t stack_hwm;    ///< Stack high-water mark (words remaining)
} TaskRecord_t;

// ─── Private State ────────────────────────────────────────────────────────────

static TaskRecord_t s_tasks[TASK_COUNT] = {
    [TASK_SENSOR]         = { "SensorTask",    false, true, 0, 0, 0 },
    [TASK_COMM]           = { "CommTask",      false, true, 0, 0, 0 },
    [TASK_OLED]           = { "OledUITask",    false, true, 0, 0, 0 },
    [TASK_HEALTH_MONITOR] = { "HealthMonitor", false, true, 0, 0, 0 },
    [TASK_RECOVERY]       = { "RecoveryMgr",   false, true, 0, 0, 0 },
    [TASK_LOGGING]        = { "LoggingTask",   false, true, 0, 0, 0 },
};

/** Task handles indexed by TaskID_t (populated once tasks are running) */
static TaskHandle_t* const s_handles[TASK_COUNT] = {
    &g_hSensorTask, &g_hCommTask,    &g_hOledTask,
    &g_hHealthMonitor, &g_hRecovery, &g_hLogging
};

// ─── Private Helpers ──────────────────────────────────────────────────────────

/**
 * @brief Send a recovery command; does not block if queue is full.
 */
static void issueRecovery(RecoveryAction_t action, TaskID_t target)
{
    RecoveryCmd_t cmd = {
        .action        = action,
        .target_task   = target,
        .retry_attempt = g_retryCount[target]
    };
    xQueueSend(g_recoveryQueue, &cmd, 0);
    xSemaphoreGive(g_faultSemaphore); // wake RecoveryManager immediately
}

/**
 * @brief Safely update the global system state under mutex.
 */
static void setSystemState(SystemState_t newState)
{
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_systemState != newState) {
            GB_LOG(TASK_HEALTH_MONITOR, LOG_INFO,
                   "State: %d → %d", g_systemState, newState);
            g_systemState = newState;
        }
        xSemaphoreGive(g_stateMutex);
    }
}

// ─── Heartbeat Check ─────────────────────────────────────────────────────────

/**
 * @brief Scan all tasks; flag misses and trigger recovery if needed.
 */
static void checkHeartbeats(void)
{
    TickType_t now     = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS);
    bool       any_fault = false;

    for (int i = 0; i < TASK_COUNT; i++) {
        TaskRecord_t* r = &s_tasks[i];

        // Skip self and recovery manager — they must NEVER be flagged
        if (i == TASK_HEALTH_MONITOR || i == TASK_RECOVERY) continue;

        TickType_t beat = g_heartbeat[i];

        // Task not yet registered — give it a grace period at boot
        if (beat == 0) continue;
        if (!r->registered) {
            r->registered = true;
            r->last_seen  = beat;
        }

        if ((now - beat) > timeout) {
            r->miss_count++;
            any_fault = true;

            if (r->miss_count == 1) {
                // First miss — warning
                GB_LOG(TASK_HEALTH_MONITOR, LOG_WARNING,
                       "[%s] heartbeat miss #1 (%lums ago)",
                       r->name, (now - beat) * portTICK_PERIOD_MS);
                xEventGroupSetBits(g_healthEvents, (1 << i));
                r->healthy = false;
            }

            // Escalating misses
            if (r->miss_count >= ESCALATION_MISS_COUNT) {
                GB_LOG(TASK_HEALTH_MONITOR, LOG_ERROR,
                       "[%s] FROZEN — %d misses, issuing recovery",
                       r->name, r->miss_count);
                g_retryCount[i]++;

                if (g_retryCount[i] <= RECOVERY_RETRY_1) {
                    setSystemState(STATE_NERVOUS);
                    issueRecovery(RECOVERY_RESTART_TASK, (TaskID_t)i);
                } else if (g_retryCount[i] <= RECOVERY_RETRY_2) {
                    setSystemState(STATE_CRITICAL);
                    issueRecovery(RECOVERY_REINIT_COMMS, (TaskID_t)i);
                } else {
                    setSystemState(STATE_CRITICAL);
                    issueRecovery(RECOVERY_SOFT_REBOOT, (TaskID_t)i);
                }
                r->miss_count = 0; // reset after issuing command
            }
        } else {
            // Task is alive — clear fault flags if previously unhealthy
            if (!r->healthy) {
                GB_LOG(TASK_HEALTH_MONITOR, LOG_INFO,
                       "[%s] heartbeat restored", r->name);
                r->healthy    = true;
                r->miss_count = 0;
                g_retryCount[i] = 0;
                xEventGroupClearBits(g_healthEvents, (1 << i));
            }
            r->last_seen = beat;
        }
    }

    // If no faults detected and we were in NERVOUS/RECOVERING, return to HEALTHY
    if (!any_fault) {
        if (g_systemState == STATE_NERVOUS ||
            g_systemState == STATE_RECOVERING) {
            setSystemState(STATE_HEALTHY);
        }
    }
}

// ─── Sensor Data Validation ───────────────────────────────────────────────────

/**
 * @brief Drain the sensor queue and validate each packet.
 *        Anomalous readings raise warnings but don't freeze the task.
 */
static void validateSensorData(void)
{
    SensorData_t data;
    while (xQueuePeek(g_sensorDataQueue, &data, 0) == pdTRUE) {
        // Consume the peeked item
        xQueueReceive(g_sensorDataQueue, &data, 0);

        if (!data.data_valid) {
            GB_LOG(TASK_HEALTH_MONITOR, LOG_WARNING,
                   "[SensorTask] received INVALID data packet");
            continue;
        }
        if (data.temperature < SENSOR_TEMP_MIN_C ||
            data.temperature > SENSOR_TEMP_MAX_C) {
            GB_LOG(TASK_HEALTH_MONITOR, LOG_WARNING,
                   "[SensorTask] temperature %.1f°C out of range",
                   data.temperature);
        }
    }
}

// ─── Memory Monitoring ───────────────────────────────────────────────────────

/**
 * @brief Check stack high-water marks for all tasks.
 *        Warns when remaining stack drops below STACK_WATERMARK_WARN.
 */
static void checkMemory(void)
{
    TaskHandle_t* const handles[TASK_COUNT] = {
        &g_hSensorTask, &g_hCommTask,    &g_hOledTask,
        &g_hHealthMonitor, &g_hRecovery, &g_hLogging
    };

    for (int i = 0; i < TASK_COUNT; i++) {
        TaskHandle_t h = *(handles[i]);
        if (h == NULL) continue;

        UBaseType_t hwm = uxTaskGetStackHighWaterMark(h);
        s_tasks[i].stack_hwm = hwm;

        if (hwm < STACK_WATERMARK_WARN) {
            GB_LOG(TASK_HEALTH_MONITOR, LOG_WARNING,
                   "[%s] stack low: %u words remaining",
                   s_tasks[i].name, (unsigned)hwm);
            xEventGroupSetBits(g_healthEvents, EVT_MEMORY_WARN);
        }
    }
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

/**
 * @brief Health Monitor main loop.
 *
 * Runs every 500 ms on CORE_CRITICAL. Priority is the highest in the
 * system so it preempts all application tasks.
 */
void healthMonitorTask(void* pvParameters)
{
    // Register with ESP32 hardware watchdog
    esp_task_wdt_add(NULL);

    GB_LOG(TASK_HEALTH_MONITOR, LOG_INFO, "HealthMonitor online");

    // Give other tasks ~2 s to boot and register their first heartbeat
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;) {
        // ── Core health checks ────────────────────────────────────────────
        checkHeartbeats();
        validateSensorData();

        // ── Memory check every 5 s (less frequent) ────────────────────────
        static uint32_t s_memTick = 0;
        if (++s_memTick >= 10) {
            s_memTick = 0;
            checkMemory();
        }

        // ── Feed hardware watchdog ────────────────────────────────────────
        esp_task_wdt_reset();

        // ── Update own heartbeat ──────────────────────────────────────────
        HEARTBEAT(TASK_HEALTH_MONITOR);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
