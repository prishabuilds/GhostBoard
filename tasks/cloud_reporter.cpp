/**
 * @file    cloud_reporter.cpp
 * @brief   Cloud Reporter Task — sends GhostBoard telemetry to Supabase
 *          over WiFi using the Supabase REST API (HTTP POST).
 *
 * WHAT IT SENDS (every 2 seconds)
 * ─────────────────────────────────
 *  1. Latest sensor reading      → sensor_data table
 *  2. All task heartbeat states  → task_status table (upsert)
 *  3. Any pending log entries    → logs table (batch up to 10)
 *  4. State transitions          → state_history table
 *
 * SETUP
 * ─────
 *  1. Create a Supabase project at https://supabase.com (free)
 *  2. Run supabase/schema.sql in the SQL Editor
 *  3. Fill in SUPABASE_URL and SUPABASE_ANON_KEY below
 *  4. Fill in your WiFi credentials
 */

#include "cloud_reporter.h"
#include "../config/system_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7 — add to platformio.ini: bblanchon/ArduinoJson@^7.0.0

// ─── USER CONFIGURATION ──────────────────────────────────────────────────────
// !! Fill these in before flashing !!

#define WIFI_SSID        "Your WIFI-SSID"  
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"   
#define SUPABASE_URL     "https://YOUR_PROJECT_ID.supabase.co"
#define SUPABASE_ANON_KEY "aeyJhbGci..."

// ─── Endpoints ────────────────────────────────────────────────────────────────
#define EP_SENSOR   SUPABASE_URL "/rest/v1/sensor_data"
#define EP_LOGS     SUPABASE_URL "/rest/v1/logs"
#define EP_TASKS    SUPABASE_URL "/rest/v1/task_status"
#define EP_HISTORY  SUPABASE_URL "/rest/v1/state_history"

// ─── Task name lookup ─────────────────────────────────────────────────────────
static const char* TASK_NAME_STR[TASK_COUNT] = {
    "SensorTask", "CommTask", "OledUITask",
    "HealthMonitor", "RecoveryMgr", "LoggingTask"
};

static const char* STATE_STR[] = { "HEALTHY", "NERVOUS", "CRITICAL", "RECOVERING" };

// ─── Last known state for transition detection ────────────────────────────────
static SystemState_t s_lastState = STATE_HEALTHY;

// ─── Private: HTTP POST helper ────────────────────────────────────────────────

/**
 * @brief POST a JSON string to a Supabase REST endpoint.
 *        Uses "Prefer: resolution=merge-duplicates" for upserts.
 *
 * @param endpoint  Full URL of the table endpoint
 * @param body      JSON string to send
 * @param upsert    true to send Prefer: merge-duplicates header
 * @return HTTP response code, or -1 on WiFi/connection failure
 */
static int supabasePost(const char* endpoint, const String& body, bool upsert = false)
{
    if (WiFi.status() != WL_CONNECTED) return -1;

    HTTPClient http;
    http.begin(endpoint);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_ANON_KEY);
    http.addHeader("Authorization", "Bearer " SUPABASE_ANON_KEY);
    if (upsert) {
        http.addHeader("Prefer", "resolution=merge-duplicates");
    }

    int code = http.POST(body);
    http.end();
    return code;
}

// ─── Private: Send sensor data ────────────────────────────────────────────────

static void sendSensorData(void)
{
    SensorData_t data;
    if (xQueuePeek(g_sensorDataQueue, &data, 0) != pdTRUE) return;

    JsonDocument doc;
    doc["temperature"]  = serialized(String(data.temperature, 2));
    doc["data_valid"]   = data.data_valid;
    doc["system_state"] = (int)g_systemState;
    doc["free_heap"]    = (int)esp_get_free_heap_size();
    doc["uptime_ms"]    = (long long)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    String body;
    serializeJson(doc, body);
    int code = supabasePost(EP_SENSOR, body);

    if (code != 201) {
        GB_LOG(TASK_COMM, LOG_WARNING, "Supabase sensor POST failed: %d", code);
    }
}

// ─── Private: Send task status (upsert) ──────────────────────────────────────

static void sendTaskStatus(void)
{
    // Build a JSON array for bulk upsert
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    TickType_t now     = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS);

    for (int i = 0; i < TASK_COUNT; i++) {
        JsonObject obj  = arr.add<JsonObject>();
        obj["task_name"]  = TASK_NAME_STR[i];
        bool healthy      = (g_heartbeat[i] != 0) &&
                            ((now - g_heartbeat[i]) <= timeout);
        obj["is_healthy"] = healthy;
        obj["miss_count"] = (int)g_retryCount[i]; // reuse as proxy
        obj["retry_count"]= (int)g_retryCount[i];
    }

    String body;
    serializeJson(doc, body);
    supabasePost(EP_TASKS, body, true /* upsert */);
}

// ─── Private: Drain log queue → Supabase ─────────────────────────────────────

static const char* SEVERITY_STR[] = {
    "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"
};

static void sendPendingLogs(void)
{
    // Collect up to 10 log entries per cycle (avoid long HTTP sessions)
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    int count = 0;

    LogEntry_t entry;
    // Peek then re-queue trick: we consume from g_logQueue here.
    // In production, use a separate cloud-log queue to avoid losing Serial logs.
    while (count < 10 &&
           xQueueReceive(g_logQueue, &entry, 0) == pdTRUE) {
        JsonObject obj     = arr.add<JsonObject>();
        obj["timestamp_ms"]= (long long)entry.timestamp_ms;
        obj["source"]      = TASK_NAME_STR[entry.source < TASK_COUNT
                                           ? entry.source : 0];
        obj["severity"]    = (int)entry.severity;
        obj["message"]     = entry.message;
        count++;
    }

    if (count == 0) return;

    String body;
    serializeJson(doc, body);
    int code = supabasePost(EP_LOGS, body);
    if (code != 201) {
        GB_LOG(TASK_COMM, LOG_WARNING, "Supabase logs POST failed: %d", code);
    }
}

// ─── Private: Detect and send state transitions ───────────────────────────────

static void sendStateTransition(void)
{
    SystemState_t current = g_systemState;
    if (current == s_lastState) return;

    JsonDocument doc;
    doc["from_state"]     = (int)s_lastState;
    doc["to_state"]       = (int)current;
    doc["trigger_reason"] = String(STATE_STR[s_lastState]) +
                            " -> " + STATE_STR[current];

    String body;
    serializeJson(doc, body);
    supabasePost(EP_HISTORY, body);

    s_lastState = current;
}

// ─── WiFi Connection ──────────────────────────────────────────────────────────

static bool connectWiFi(void)
{
    GB_LOG(TASK_COMM, LOG_INFO, "Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        GB_LOG(TASK_COMM, LOG_INFO, "WiFi connected. IP: %s",
               WiFi.localIP().toString().c_str());
        return true;
    }

    GB_LOG(TASK_COMM, LOG_ERROR, "WiFi connection FAILED");
    return false;
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

void cloudReporterTask(void* pvParameters)
{
    GB_LOG(TASK_COMM, LOG_INFO, "CloudReporter starting...");

    // Attempt WiFi — retry every 30 s if failed
    while (!connectWiFi()) {
        HEARTBEAT(TASK_COMM);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }

    GB_LOG(TASK_COMM, LOG_INFO, "CloudReporter online — streaming to Supabase");

    for (;;) {
        // Reconnect if dropped
        if (WiFi.status() != WL_CONNECTED) {
            GB_LOG(TASK_COMM, LOG_WARNING, "WiFi lost — reconnecting");
            connectWiFi();
        }

        sendSensorData();
        sendTaskStatus();
        sendPendingLogs();
        sendStateTransition();

        HEARTBEAT(TASK_COMM);
        vTaskDelay(pdMS_TO_TICKS(2000)); // report every 2 seconds
    }
}
