/**
 * ╔═══════════════════════════════════════════════════════════╗
 * ║             G H O S T B O A R D  v1.0                    ║
 * ║       AI-Assisted Self-Healing Embedded System            ║
 * ║   Fault-tolerant FreeRTOS architecture on ESP32           ║
 * ╚═══════════════════════════════════════════════════════════╝
 *
 * @file    main.cpp
 * @author  GhostBoard Project
 * @brief   System entry point — initializes hardware, shared
 *          resources, and launches all RTOS tasks.
 *
 * BOOT SEQUENCE
 * ─────────────
 *  1. Hardware init (UART, I2C, SPI, GPIO)
 *  2. Shared resource creation (queues, semaphores, event groups)
 *  3. System state initialization
 *  4. RTOS task creation (priority-ordered)
 *  5. Scheduler start
 */

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "config/system_config.h"
#include "monitoring/health_monitor.h"
#include "recovery/recovery_manager.h"
#include "tasks/sensor_task.h"
#include "tasks/comm_task.h"
#include "tasks/logging_task.h"
#include "ui/oled_ui_task.h"
#include "drivers/hardware_init.h"

// ─── Shared FreeRTOS Objects (extern-declared in system_config.h) ────────────

/** Sensor data queue: SensorTask → HealthMonitor / CommTask */
QueueHandle_t   g_sensorDataQueue   = NULL;

/** Log entry queue: all tasks → LoggingTask */
QueueHandle_t   g_logQueue          = NULL;

/** Recovery command queue: HealthMonitor → RecoveryManager */
QueueHandle_t   g_recoveryQueue     = NULL;

/** Protects g_systemState and heartbeat table */
SemaphoreHandle_t g_stateMutex      = NULL;

/** Binary semaphore: wakes RecoveryManager on fault */
SemaphoreHandle_t g_faultSemaphore  = NULL;

/** Tracks task health events (bits defined in system_config.h) */
EventGroupHandle_t g_healthEvents   = NULL;

/** Global system state (HEALTHY / NERVOUS / CRITICAL / RECOVERING) */
volatile SystemState_t g_systemState = STATE_HEALTHY;

/** Per-task heartbeat timestamps */
volatile TickType_t g_heartbeat[TASK_COUNT] = {0};

/** Per-task recovery retry counters */
volatile uint8_t g_retryCount[TASK_COUNT] = {0};

// ─── Task Handles (used by RecoveryManager to suspend/resume/restart) ────────
TaskHandle_t g_hSensorTask    = NULL;
TaskHandle_t g_hCommTask      = NULL;
TaskHandle_t g_hOledTask      = NULL;
TaskHandle_t g_hHealthMonitor = NULL;
TaskHandle_t g_hRecovery      = NULL;
TaskHandle_t g_hLogging       = NULL;

// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Create all FreeRTOS shared resources.
 * @return true on success, false if any allocation failed.
 */
static bool createSharedResources(void)
{
    g_sensorDataQueue = xQueueCreate(SENSOR_QUEUE_SIZE,   sizeof(SensorData_t));
    g_logQueue        = xQueueCreate(LOG_QUEUE_SIZE,      sizeof(LogEntry_t));
    g_recoveryQueue   = xQueueCreate(RECOVERY_QUEUE_SIZE, sizeof(RecoveryCmd_t));

    g_stateMutex    = xSemaphoreCreateMutex();
    g_faultSemaphore = xSemaphoreCreateBinary();
    g_healthEvents  = xEventGroupCreate();

    // Verify all resources allocated
    if (!g_sensorDataQueue || !g_logQueue   || !g_recoveryQueue  ||
        !g_stateMutex      || !g_faultSemaphore || !g_healthEvents) {
        Serial.println("[BOOT] FATAL: shared resource allocation failed");
        return false;
    }
    return true;
}

/**
 * @brief Spawn all RTOS tasks in priority order.
 *        Critical infrastructure tasks get higher priority.
 */
static void createTasks(void)
{
    // ── CRITICAL TIER (must always run) ──────────────────────────────────────

    xTaskCreatePinnedToCore(
        healthMonitorTask,           // task function
        "HealthMonitor",             // name
        STACK_HEALTH_MONITOR,        // stack size (words)
        NULL,                        // parameter
        PRIO_HEALTH_MONITOR,         // priority
        &g_hHealthMonitor,           // handle
        CORE_CRITICAL                // core (0)
    );

    xTaskCreatePinnedToCore(
        recoveryManagerTask,
        "RecoveryMgr",
        STACK_RECOVERY_MANAGER,
        NULL,
        PRIO_RECOVERY_MANAGER,
        &g_hRecovery,
        CORE_CRITICAL
    );

    // ── APPLICATION TIER ─────────────────────────────────────────────────────

    xTaskCreatePinnedToCore(
        sensorTask,
        "SensorTask",
        STACK_SENSOR_TASK,
        NULL,
        PRIO_SENSOR_TASK,
        &g_hSensorTask,
        CORE_APP
    );

    xTaskCreatePinnedToCore(
        commTask,
        "CommTask",
        STACK_COMM_TASK,
        NULL,
        PRIO_COMM_TASK,
        &g_hCommTask,
        CORE_APP
    );

    xTaskCreatePinnedToCore(
        loggingTask,
        "LoggingTask",
        STACK_LOGGING_TASK,
        NULL,
        PRIO_LOGGING_TASK,
        &g_hLogging,
        CORE_APP
    );

    // ── NON-CRITICAL UI TIER ─────────────────────────────────────────────────

    xTaskCreatePinnedToCore(
        oledUITask,
        "OledUITask",
        STACK_OLED_TASK,
        NULL,
        PRIO_OLED_TASK,
        &g_hOledTask,
        CORE_APP
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup() — runs once before scheduler starts
// ─────────────────────────────────────────────────────────────────────────────
void setup(void)
{
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(100)); // let serial settle

    Serial.println("\n╔══════════════════════════════╗");
    Serial.println("║   GhostBoard  v1.0  BOOT     ║");
    Serial.println("╚══════════════════════════════╝");

    // Step 1: Hardware peripherals
    if (!hardware_init()) {
        Serial.println("[BOOT] Hardware init FAILED — halting");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Step 2: Shared resources
    if (!createSharedResources()) {
        Serial.println("[BOOT] Resource allocation FAILED — halting");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Step 3: Spawn tasks
    createTasks();

    Serial.println("[BOOT] All tasks created — scheduler active");
    // FreeRTOS scheduler is already running under Arduino/ESP-IDF
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino loop() — idle task under FreeRTOS (keep lightweight)
// ─────────────────────────────────────────────────────────────────────────────
void loop(void)
{
    // Feed ESP32 hardware watchdog from the idle context
    // (actual logic watchdog is managed by HealthMonitor)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
