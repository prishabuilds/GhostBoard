/**
 * @file    fault_inject.cpp
 * @brief   Global definition of the fault injection control struct.
 *          All fields default to false (no fault injected at boot).
 *
 * Set fields to true over Serial to simulate faults:
 *   Send 'F1' → freeze sensor task
 *   Send 'F2' → corrupt sensor data
 *   Send 'F3' → stall comm task
 *   Send 'F0' → clear all faults
 */

#include "../config/system_config.h"

volatile FaultInjection_t g_faultInject = {
    .freeze_sensor   = false,
    .corrupt_sensor  = false,
    .comm_timeout    = false,
    .memory_pressure = false,
};
