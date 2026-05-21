/**
 * @file    comm_task.h
 * @brief   Public interface for the Communication Task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void commTask(void* pvParameters);
