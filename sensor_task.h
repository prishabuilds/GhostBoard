/**
 * @file    sensor_task.h
 * @brief   Public interface for the Sensor Task.
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sensorTask(void* pvParameters);
