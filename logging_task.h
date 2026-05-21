/**
 * @file    logging_task.h
 * @brief   Public interface for the Logging Task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void loggingTask(void* pvParameters);
