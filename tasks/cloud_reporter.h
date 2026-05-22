/**
 * @file    cloud_reporter.h
 * @brief   Cloud Reporter Task — streams GhostBoard telemetry to Supabase.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief FreeRTOS task that POSTs sensor data, logs, task status,
 *        and state transitions to Supabase over WiFi.
 *
 * Replaces the stub CommTask when WiFi is available.
 * Add to task creation in main.cpp with PRIO_COMM_TASK priority.
 */
void cloudReporterTask(void* pvParameters);
