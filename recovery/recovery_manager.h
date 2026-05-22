/**
 * @file    recovery_manager.h
 * @brief   Public interface for the Recovery Manager task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * @brief FreeRTOS task function for the Recovery Manager.
 *        Create via xTaskCreatePinnedToCore() — do not call directly.
 */
void recoveryManagerTask(void* pvParameters);
