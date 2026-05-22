👻 GhostBoard — AI-Assisted Self-Healing Embedded System
> *A fault-tolerant FreeRTOS firmware that detects, isolates, and recovers from hardware/software failures autonomously — without rebooting the device.*
![Platform](https://img.shields.io/badge/Platform-ESP32-blue?style=flat-square)
![Framework](https://img.shields.io/badge/Framework-FreeRTOS-green?style=flat-square)
![Language](https://img.shields.io/badge/Language-C%2FC%2B%2B-orange?style=flat-square)
![Build](https://img.shields.io/badge/Build-PlatformIO-purple?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square)
---
📖 Table of Contents
Overview
Architecture
Task Descriptions
System State Machine
Fault Simulation
Recovery Logic
Hardware Setup
Pin Connections
Build & Flash
File Structure
Future Improvements
---
Overview
GhostBoard is a next-generation resilient embedded platform built on ESP32 + FreeRTOS.  
It behaves like a living digital organism:
🫀 Heartbeat monitoring — every task reports liveness every second
🔍 Anomaly detection — sensor data validated in real-time
🛠️ Autonomous recovery — failed tasks are restarted without device reboot
📟 Cyberpunk OLED dashboard — live system state, temperature, memory usage
📝 Persistent logging — timestamped logs to Serial + SD card
💀 Fault injection — simulate real failures via Serial commands
---
Architecture
```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32 (Dual Core)                     │
│                                                             │
│  CORE 0 — Critical Tier          CORE 1 — Application Tier  │
│  ┌──────────────────────┐        ┌───────────────────────┐  │
│  │  Health Monitor      │◄──────►│  Sensor Task          │  │
│  │  (heartbeat polling, │        │  (ADC read, queue)    │  │
│  │   anomaly detect,    │        ├───────────────────────┤  │
│  │   state machine)     │        │  Comm Task            │  │
│  ├──────────────────────┤        │  (UART/WiFi/MQTT sim) │  │
│  │  Recovery Manager    │        ├───────────────────────┤  │
│  │  (task restart,      │        │  Logging Task         │  │
│  │   module reinit,     │        │  (Serial + SD card)   │  │
│  │   soft reboot)       │        ├───────────────────────┤  │
│  └──────────────────────┘        │  OLED UI Task         │  │
│                                  │  (SSD1306 dashboard)  │  │
│                                  └───────────────────────┘  │
│                                                             │
│  Shared Resources:  Queues │ Semaphores │ Event Groups      │
└─────────────────────────────────────────────────────────────┘
```
Shared FreeRTOS Objects
Object	Type	Purpose
`g_sensorDataQueue`	Queue (10)	Sensor packets → HealthMonitor / CommTask
`g_logQueue`	Queue (32)	Log entries → LoggingTask
`g_recoveryQueue`	Queue (8)	Recovery commands → RecoveryManager
`g_stateMutex`	Mutex	Protects `g_systemState`
`g_faultSemaphore`	Binary Semaphore	Wakes RecoveryManager on fault
`g_healthEvents`	Event Group	Per-task fault bits + memory warning
`g_heartbeat[]`	Volatile Array	Per-task last-seen tick timestamps
`g_retryCount[]`	Volatile Array	Per-task recovery attempt counter
---
Task Descriptions
1. 🩺 Health Monitor (Core 0, Priority 8)
The central nervous system. Runs every 500 ms.
Compares `xTaskGetTickCount()` against `g_heartbeat[taskID]`
Flags any task silent for more than 3000 ms
Validates sensor data for out-of-range values
Checks stack high-water marks for overflow risk
Issues `RecoveryCmd_t` to the recovery queue
Feeds the ESP32 hardware watchdog
2. 🔧 Recovery Manager (Core 0, Priority 7)
Blocks on `g_faultSemaphore`. Wakes only when a fault is detected.
Attempt	Action
1st	Restart the offending task
2nd	Re-initialize hardware module / clear queues
3rd	System soft reboot (`esp_restart()`)
3. 🌡️ Sensor Task (Core 1, Priority 5)
Reads NTC thermistor via 12-bit ADC (GPIO 34)
Converts raw ADC → °C using Steinhart-Hart equation
Sends `SensorData_t` packets to `g_sensorDataQueue`
Reports heartbeat every 1 second
4. 📡 Comm Task (Core 1, Priority 4)
Simulates UART/WiFi/MQTT telemetry
Serializes sensor data to JSON string
Implements 3-attempt retry with 500 ms delay
Simulates 5% random packet loss
5. 📝 Logging Task (Core 1, Priority 3)
Drains `g_logQueue` continuously
Writes to Serial (always) + SD card (if mounted)
Format: `[TIME_MS] [MODULE] [SEVERITY] MESSAGE`
Flushes SD every 10 entries
6. 🖥️ OLED UI Task (Core 1, Priority 2)
Drives 128×64 SSD1306 over I2C
Refreshes every 250 ms
Displays: system state, task count, fault count, temperature, memory bar
Animations: blink (NERVOUS), invert flash (CRITICAL), scrolling dots (RECOVERING)
---
System State Machine
```
                ┌─────────────────────────────┐
                ▼                             │
           ┌─────────┐   heartbeat miss   ┌──────────┐
    boot ──►│ HEALTHY │──────────────────►│ NERVOUS  │
           └─────────┘                   └──────────┘
                ▲                             │
                │                    escalation threshold
                │                             │
                │                             ▼
          recovery OK               ┌──────────────┐
                │                   │   CRITICAL   │
                │                   └──────────────┘
                │                             │
                │                  recovery cmd issued
                │                             │
                │                             ▼
                └──────────────── ┌─────────────────┐
                                  │   RECOVERING    │
                                  └─────────────────┘
```
LED Indicators:
State	Green LED	Yellow LED	Red LED	Buzzer
HEALTHY	✅ ON	OFF	OFF	Silent
NERVOUS	OFF	✅ ON	OFF	Silent
RECOVERING	OFF	✅ ON	OFF	Silent
CRITICAL	OFF	OFF	✅ ON	🔔 Beep
---
Fault Simulation
Send these commands via Serial monitor (115200 baud) to simulate real failures:
Command	Fault Injected	What GhostBoard Does
`F1`	SensorTask frozen	Heartbeat stops → HealthMonitor detects after 3s → RecoveryManager restarts task
`F2`	Corrupt sensor data	Invalid packets queued → HealthMonitor raises WARNING
`F3`	CommTask stalled	Comm heartbeat stops → recovery restarts comm module
`F0`	Clear all faults	All flags reset → system returns to HEALTHY
---
Recovery Logic (Escalation Flow)
```
SensorTask frozen detected
         │
         ▼
   retry_count == 1
   → NERVOUS state
   → Restart SensorTask
         │
         ▼ (if still failing)
   retry_count == 2
   → CRITICAL state
   → Reinit hardware module + clear queues
         │
         ▼ (if still failing)
   retry_count == 3
   → Log "MAX retries exceeded"
   → esp_restart() soft reboot
```
---
Hardware Setup
Required Components
Component	Purpose
ESP32 DevKit v1	Main MCU
SSD1306 OLED (128×64, I2C)	System dashboard
NTC Thermistor 10kΩ + 10kΩ resistor	Temperature sensing
MicroSD card module (SPI)	Persistent log storage
3× LEDs (green, yellow, red) + resistors	Status indication
Active buzzer	Critical alert
---
Pin Connections
```
ESP32 GPIO  →  Component
──────────────────────────────────────────
GPIO 2      →  Green LED (anode via 220Ω)
GPIO 4      →  Yellow LED (anode via 220Ω)
GPIO 5      →  Red LED (anode via 220Ω)
GPIO 18     →  Active Buzzer (+)
GPIO 34     →  NTC Thermistor voltage divider (midpoint)
GPIO 21     →  OLED SDA (I2C)
GPIO 22     →  OLED SCL (I2C)
GPIO 15     →  SD Card CS (SPI)
GPIO 23     →  SD Card MOSI (SPI)
GPIO 19     →  SD Card MISO (SPI)
GPIO 14     →  SD Card SCK (SPI)
3.3V        →  OLED VCC, SD VCC, Thermistor top
GND         →  All GND connections
```
Thermistor voltage divider:
```
3.3V ──[ 10kΩ ]──┬──[ NTC ]── GND
                 │
              GPIO 34
```
---
Build & Flash
Prerequisites
PlatformIO (VS Code extension or CLI)
ESP32 board connected via USB
Steps
```bash
# 1. Clone the repository
git clone https://github.com/YOUR_USERNAME/GhostBoard.git
cd GhostBoard

# 2. Build firmware
pio run

# 3. Flash to ESP32
pio run --target upload

# 4. Open Serial monitor
pio device monitor --baud 115200
```
Expected Serial Output
```
╔══════════════════════════════╗
║   GhostBoard  v1.0  BOOT     ║
╚══════════════════════════════╝
[HW] Peripherals initialized
[BOOT] All tasks created — scheduler active
[      50ms] [HealthMonitor] [INFO    ] HealthMonitor online
[      52ms] [SensorTask   ] [INFO    ] SensorTask online
[      54ms] [CommTask      ] [INFO    ] CommTask online
[      56ms] [LoggingTask   ] [INFO    ] LoggingTask online
[      58ms] [OledUITask    ] [INFO    ] OledUITask online
[      60ms] [RecoveryMgr   ] [INFO    ] RecoveryManager online — awaiting faults
```
---
File Structure
```
GhostBoard/
│
├── firmware/
│   ├── main.cpp                  ← Boot sequence, task creation
│   └── config/
│       ├── system_config.h       ← ALL types, constants, extern declarations
│       ├── fault_inject.h        ← Fault injection extern header
│       └── fault_inject.cpp      ← Fault injection global definition
│
├── monitoring/
│   ├── health_monitor.h
│   └── health_monitor.cpp        ← Heartbeat polling, anomaly detection
│
├── recovery/
│   ├── recovery_manager.h
│   └── recovery_manager.cpp      ← Task restart, module reinit, soft reboot
│
├── tasks/
│   ├── sensor_task.h / .cpp      ← ADC read, Steinhart-Hart, queue push
│   ├── comm_task.h / .cpp        ← JSON telemetry, retry logic
│   └── logging_task.h / .cpp     ← Serial + SD card logging
│
├── ui/
│   ├── oled_ui_task.h
│   └── oled_ui_task.cpp          ← SSD1306 cyberpunk dashboard
│
├── drivers/
│   ├── hardware_init.h
│   └── hardware_init.cpp         ← GPIO, ADC, SPI, watchdog init
│
├── testing/
│   └── fault_inject.cpp          ← Global FaultInjection_t definition
│
├── platformio.ini                ← PlatformIO build config
├── .gitignore
└── README.md
```
---
Future Improvements
Phase	Feature	Description
6	TinyML Anomaly Detection	Train Edge Impulse model on normal sensor/timing data; run TFLite Micro on ESP32 for intelligent anomaly scoring
7	OTA Self-Update	Pull firmware patches over WiFi when healthy; apply only after integrity check
8	Multi-Node Mesh	ESP-NOW mesh between multiple GhostBoards — distributed health consensus
9	Web Dashboard	Serve real-time system telemetry via ESP32 WiFi AP (WebSocket + HTML5)
10	Persistent Fault History	Store recovery events in NVS flash; analyze MTBF across reboots
---
License
MIT License — see LICENSE for details.
---
GhostBoard — Built to survive.
