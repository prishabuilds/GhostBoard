/**
 * @file    recovery_manager.cpp
 * @brief   Recovery Manager Task — autonomous fault recovery engine.
 *
 * RESPONSIBILITIES
 * ────────────────
 *  • Block on g_faultSemaphore — woken only when HealthMonitor finds a fault.
 *  • Drain g_recoveryQueue and execute each recovery command.
 *  • Apply escalating recovery strategies:
 *      Attempt 1 → restart the offending task
 *      Attempt 2 → re-initialize the hardware module / clear queues
 *      Attempt 3 → system soft-reboot (esp_restart)
 *  • Log every recovery action and its outcome.
 *  • Signal RECOVERING state during recovery; restore HEALTHY on success.
 *
 * DESIGN NOTES
 * ────────────
 *  This task deliberately runs at high priority (just below HealthMonitor)
 *  so recovery actions execute before lower-priority application tasks
 *  consume stale state. All task restarts use the cached handles and
 *  recreate tasks with the same parameters to avoid re-entering setup().
 */

#include "recovery_manager.h"
#include "../config/system_config.h"
#include "esp_system.h"     // esp_restart()

// ─── Task Restart Descriptors ─────────────────────────────────────────────────

/**
 * @brief Parameters needed to recreate a task after killing it.
 *        Mirrors the xTaskCreatePinnedToCore() call in main.cpp.
 */
typedef struct {
    TaskFunction_t  func;
    const char*     name;
    uint32_t        stack;
    UBaseType_t     priority;
    BaseType_t      core;
    TaskHandle_t**  handle_ptr; ///< Pointer to the handle pointer in main.cpp
} TaskDescriptor_t;

// Forward declarations of task functions (defined in their own files)
extern void sensorTask(void*);
extern void commTask(void*);
extern void oledUITask(void*);
extern void loggingTask(void*);

static const TaskDescriptor_t s_taskDesc[TASK_COUNT] = {
    [TASK_SENSOR] = {
        sensorTask, "SensorTask", STACK_SENSOR_TASK,
        PRIO_SENSOR_TASK, CORE_APP, &g_hSensorTask
    },
    [TASK_COMM] = {
        commTask, "CommTask", STACK_COMM_TASK,
        PRIO_COMM_TASK, CORE_APP, &g_hCommTask
    },
    [TASK_OLED] = {
        oledUITask, "OledUITask", STACK_OLED_TASK,
        PRIO_OLED_TASK, CORE_APP, &g_hOledTask
    },
    [TASK_LOGGING] = {
        loggingTask, "LoggingTask", STACK_LOGGING_TASK,
        PRIO_LOGGING_TASK, CORE_APP, &g_hLogging
    },
    // Health Monitor and Recovery Manager never restart themselves
    [TASK_HEALTH_MONITOR] = { NULL },
    [TASK_RECOVERY]       = { NULL },
};

// ─── Recovery Actions ─────────────────────────────────────────────────────────

/**
 * @brief Kill and recreate a single task.
 *        The old task handle is deleted first, then a fresh task is spawned
 *        with identical parameters. The g_heartbeat entry is zeroed so the
 *        HealthMonitor gives the new instance a boot grace period.
 *
 * @param id  TaskID_t of the task to restart.
 * @return    true if the new task was created successfully.
 */
static bool restartTask(TaskID_t id)
{
    const TaskDescriptor_t* d = &s_taskDesc[id];
    if (d->func == NULL) {
        GB_LOG(TASK_RECOVERY, LOG_ERROR,
               "restartTask: task %d has no descriptor", id);
        return false;
    }

    GB_LOG(TASK_RECOVERY, LOG_INFO,
           "Restarting task [%s] (attempt %d)", d->name, g_retryCount[id]);

    // 1. Delete old task if still alive
    TaskHandle_t old = *(d->handle_ptr);
    if (old != NULL) {
        vTaskDelete(old);
        *(d->handle_ptr) = NULL;
        vTaskDelay(pdMS_TO_TICKS(50)); // let scheduler clean up
    }

    // 2. Reset heartbeat so HealthMonitor grants a grace period
    g_heartbeat[id] = xTaskGetTickCount();

    // 3. Recreate task
    BaseType_t result = xTaskCreatePinnedToCore(
        d->func, d->name, d->stack, NULL,
        d->priority, d->handle_ptr, d->core
    );

    if (result != pdPASS) {
        GB_LOG(TASK_RECOVERY, LOG_CRITICAL,
               "FAILED to recreate [%s] — heap exhausted?", d->name);
        return false;
    }

    GB_LOG(TASK_RECOVERY, LOG_INFO,
           "[%s] successfully restarted", d->name);
    return true;
}

/**
 * @brief Reinitialize communication subsystem without full restart.
 *        Simulated here; in production this would call WiFi/UART re-init APIs.
 */
static void reinitComms(void)
{
    GB_LOG(TASK_RECOVERY, LOG_INFO, "Re-initializing communications module");

    // Simulate module re-init delay
    vTaskDelay(pdMS_TO_TICKS(300));

    // Reset CommTask heartbeat to grant grace period
    g_heartbeat[TASK_COMM] = xTaskGetTickCount();

    GB_LOG(TASK_RECOVERY, LOG_INFO, "Communications module re-initialized");
}

/**
 * @brief Clear all application queues to free memory and break deadlocks.
 */
static void clearQueues(void)
{
    GB_LOG(TASK_RECOVERY, LOG_WARNING, "Clearing application queues");
    xQueueReset(g_sensorDataQueue);
    // Note: g_logQueue is NOT cleared — we want to preserve the fault log
    GB_LOG(TASK_RECOVERY, LOG_INFO, "Queues cleared");
}

/**
 * @brief Issue a full device soft-reboot. Last resort — logs the reason first.
 */
static void softReboot(void)
{
    GB_LOG(TASK_RECOVERY, LOG_CRITICAL,
           "MAX retries exceeded — initiating soft reboot");
    vTaskDelay(pdMS_TO_TICKS(200)); // Allow log entry to flush
    esp_restart();
}

// ─── Command Dispatcher ───────────────────────────────────────────────────────

/**
 * @brief Execute a single RecoveryCmd_t.
 */
static void executeRecovery(const RecoveryCmd_t* cmd)
{
    // Mark system as RECOVERING
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_systemState = STATE_RECOVERING;
        xSemaphoreGive(g_stateMutex);
    }

    bool success = false;

    switch (cmd->action) {
        case RECOVERY_RESTART_TASK:
            success = restartTask(cmd->target_task);
            break;

        case RECOVERY_REINIT_COMMS:
            reinitComms();
            success = true;
            break;

        case RECOVERY_CLEAR_QUEUES:
            clearQueues();
            success = true;
            break;

        case RECOVERY_SOFT_REBOOT:
            softReboot(); // does not return
            break;

        case RECOVERY_ESCALATE:
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_systemState = STATE_CRITICAL;
                xSemaphoreGive(g_stateMutex);
            }
            GB_LOG(TASK_RECOVERY, LOG_CRITICAL, "System escalated to CRITICAL");
            success = false; // don't restore HEALTHY
            break;

        default:
            GB_LOG(TASK_RECOVERY, LOG_ERROR,
                   "Unknown recovery action: %d", cmd->action);
            break;
    }

    if (success) {
        // Mark system as recovering; HealthMonitor will restore HEALTHY
        // once it sees heartbeats resume
        xEventGroupSetBits(g_healthEvents, EVT_RECOVERY_DONE);
    }
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

/**
 * @brief Recovery Manager main loop.
 *
 * Blocks indefinitely on g_faultSemaphore. Wakes only when HealthMonitor
 * detects a fault and gives the semaphore. Drains the entire recovery queue
 * before blocking again.
 */
void recoveryManagerTask(void* pvParameters)
{
    GB_LOG(TASK_RECOVERY, LOG_INFO, "RecoveryManager online — awaiting faults");
    HEARTBEAT(TASK_RECOVERY);

    for (;;) {
        // Block until HealthMonitor signals a fault (or 10 s timeout for safety)
        xSemaphoreTake(g_faultSemaphore, pdMS_TO_TICKS(10000));

        RecoveryCmd_t cmd;
        while (xQueueReceive(g_recoveryQueue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
            executeRecovery(&cmd);
        }

        HEARTBEAT(TASK_RECOVERY);
    }
}
