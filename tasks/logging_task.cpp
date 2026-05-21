/**
 * @file    logging_task.cpp
 * @brief   Logging Task — drains g_logQueue and writes formatted
 *          entries to Serial output and (optionally) SD card.
 *
 * LOG FORMAT
 * ──────────
 *   [TIME_MS] [MODULE_NAME] [SEVERITY] MESSAGE
 *
 * EXAMPLE OUTPUT
 * ──────────────
 *   [10234ms] [SensorTask  ] [WARNING ] Sensor timeout detected
 *   [10280ms] [RecoveryMgr ] [INFO    ] Restarted SensorTask
 *
 * SD CARD
 * ───────
 *   Writes to /ghostboard.log on SD card if mounted.
 *   Falls back to Serial-only if SD is absent.
 */

#include "logging_task.h"
#include "../config/system_config.h"
#include "SD.h"
#include "SPI.h"

// ─── Module Name Table ────────────────────────────────────────────────────────

static const char* const TASK_NAMES[TASK_COUNT] = {
    "SensorTask  ",
    "CommTask    ",
    "OledUITask  ",
    "HealthMonitor",
    "RecoveryMgr ",
    "LoggingTask ",
};

static const char* const SEVERITY_NAMES[] = {
    "DEBUG   ",
    "INFO    ",
    "WARNING ",
    "ERROR   ",
    "CRITICAL",
};

// ─── SD Card State ────────────────────────────────────────────────────────────

static bool    s_sdMounted    = false;
static File    s_logFile;
static uint32_t s_entryCount  = 0;

// ─── Private Helpers ──────────────────────────────────────────────────────────

/**
 * @brief Attempt to mount SD card and open log file.
 * @return true if SD is ready.
 */
static bool initSD(void)
{
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[LoggingTask] SD card not found — logging to Serial only");
        return false;
    }
    s_logFile = SD.open("/ghostboard.log", FILE_APPEND);
    if (!s_logFile) {
        Serial.println("[LoggingTask] Failed to open log file on SD");
        return false;
    }
    Serial.println("[LoggingTask] SD card mounted — logging to /ghostboard.log");
    return true;
}

/**
 * @brief Format and write a single LogEntry_t to all outputs.
 */
static void writeEntry(const LogEntry_t* e)
{
    char line[160];
    const char* taskName = (e->source < TASK_COUNT)
                           ? TASK_NAMES[e->source] : "Unknown     ";
    const char* sevName  = (e->severity <= LOG_CRITICAL)
                           ? SEVERITY_NAMES[e->severity] : "UNKNOWN ";

    snprintf(line, sizeof(line),
             "[%8lums] [%-13s] [%s] %s",
             (unsigned long)e->timestamp_ms,
             taskName, sevName, e->message);

    // Always write to Serial
    Serial.println(line);

    // Write to SD if mounted
    if (s_sdMounted && s_logFile) {
        s_logFile.println(line);
        // Flush every 10 entries to balance write speed vs data safety
        if ((++s_entryCount % 10) == 0) {
            s_logFile.flush();
        }
    }
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

void loggingTask(void* pvParameters)
{
    // Try to mount SD card
    s_sdMounted = initSD();

    // Write boot banner
    const char* banner =
        "\n==============================\n"
        "  GhostBoard Log — Boot\n"
        "==============================";
    Serial.println(banner);
    if (s_sdMounted && s_logFile) s_logFile.println(banner);

    GB_LOG(TASK_LOGGING, LOG_INFO, "LoggingTask online");

    for (;;) {
        LogEntry_t entry;

        // Block up to 100 ms waiting for a log entry
        if (xQueueReceive(g_logQueue, &entry, pdMS_TO_TICKS(100)) == pdTRUE) {
            writeEntry(&entry);
        }

        HEARTBEAT(TASK_LOGGING);
    }
}
