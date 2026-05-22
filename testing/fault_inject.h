/**
 * @file    fault_inject.h
 * @brief   Fault injection control — extern declaration of g_faultInject.
 *
 * Include this in any task that reads g_faultInject flags.
 * The actual definition lives in testing/fault_inject.cpp.
 *
 * USAGE (Serial commands to trigger faults at runtime)
 * ────────────────────────────────────────────────────
 *   Send over Serial monitor (115200 baud):
 *
 *   'F1'  →  freeze SensorTask      (no heartbeat → HealthMonitor detects)
 *   'F2'  →  corrupt sensor data    (invalid packets → anomaly detection)
 *   'F3'  →  stall CommTask         (comm timeout simulation)
 *   'F0'  →  clear ALL faults       (return to normal operation)
 *
 * These map directly to fields in FaultInjection_t (system_config.h).
 */

#pragma once
#include "system_config.h"

/**
 * @brief Global fault injection control register.
 *        Written by serial command handler; read by application tasks.
 */
extern volatile FaultInjection_t g_faultInject;
