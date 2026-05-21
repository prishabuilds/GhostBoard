/**
 * @file    hardware_init.h
 * @brief   Hardware initialization and status update interface.
 */

#pragma once
#include "../config/system_config.h"

bool hardware_init(void);
void hardware_updateStatus(SystemState_t state);
