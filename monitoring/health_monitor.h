/**
 * @file    health_monitor.h
 * @brief   Public interface for the Health Monitor task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief FreeRTOS task function for the Health Monitor.
 *        Create via xTaskCreatePinnedToCore() — do not call directly.
 */
void healthMonitorTask(void* pvParameters);
