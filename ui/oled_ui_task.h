/**
 * @file    oled_ui_task.h
 * @brief   Public interface for the OLED UI Task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void oledUITask(void* pvParameters);
