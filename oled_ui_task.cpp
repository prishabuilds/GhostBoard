/**
 * @file    oled_ui_task.cpp
 * @brief   OLED UI Task — drives a 128×64 SSD1306 display with a
 *          cyberpunk-style dashboard showing real-time system state.
 *
 * DISPLAY LAYOUT
 * ──────────────
 *   ┌────────────────────────────┐
 *   │ GHOSTBOARD  v1.0           │  ← header bar
 *   ├────────────────────────────┤
 *   │ STATE: ██ HEALTHY          │  ← system mood
 *   │ TASKS: 6/6  FAULTS: 0      │  ← task count / fault count
 *   │ TEMP:  27.3°C              │  ← latest sensor value
 *   │ MEM:   ████████░░  82%     │  ← heap usage bar
 *   └────────────────────────────┘
 *
 * STATE ANIMATIONS
 * ────────────────
 *  HEALTHY    → static solid block indicator
 *  NERVOUS    → slow blinking indicator
 *  CRITICAL   → fast inverted-screen flash
 *  RECOVERING → scrolling dots animation
 *
 * Requires: Adafruit_GFX + Adafruit_SSD1306 libraries.
 */

#include "oled_ui_task.h"
#include "../config/system_config.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── Display Configuration ────────────────────────────────────────────────────

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT    64
#define OLED_RESET       -1   // No reset pin on most modules

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT,
                                &Wire, OLED_RESET);

// ─── Animation State ──────────────────────────────────────────────────────────

static uint8_t  s_blinkPhase    = 0;   ///< 0 or 1 for blink animations
static uint8_t  s_scrollDot     = 0;   ///< 0-3 for recovery dots animation
static uint32_t s_faultCount    = 0;   ///< Cumulative fault counter
static float    s_lastTemp      = 0.0f;

// ─── State Label & Icon Helpers ───────────────────────────────────────────────

static const char* stateLabel(SystemState_t s)
{
    switch (s) {
        case STATE_HEALTHY:    return "HEALTHY";
        case STATE_NERVOUS:    return "NERVOUS";
        case STATE_CRITICAL:   return "CRITICAL";
        case STATE_RECOVERING: return "RECOVERING";
        default:               return "UNKNOWN";
    }
}

/**
 * @brief Draw a small 8×8 status icon at (x,y) depending on state.
 *        HEALTHY=filled square, NERVOUS=outline, CRITICAL=X, RECOVERING=arrow.
 */
static void drawStateIcon(int16_t x, int16_t y, SystemState_t s)
{
    switch (s) {
        case STATE_HEALTHY:
            display.fillRect(x, y, 8, 8, SSD1306_WHITE);
            break;
        case STATE_NERVOUS:
            display.drawRect(x, y, 8, 8, SSD1306_WHITE);
            if (s_blinkPhase) display.fillRect(x+2, y+2, 4, 4, SSD1306_WHITE);
            break;
        case STATE_CRITICAL:
            // Draw X
            display.drawLine(x,   y,   x+7, y+7, SSD1306_WHITE);
            display.drawLine(x+7, y,   x,   y+7, SSD1306_WHITE);
            break;
        case STATE_RECOVERING:
            // Draw right-pointing arrow
            display.drawLine(x, y+4, x+6, y+4, SSD1306_WHITE);
            display.drawLine(x+3, y+1, x+6, y+4, SSD1306_WHITE);
            display.drawLine(x+3, y+7, x+6, y+4, SSD1306_WHITE);
            break;
    }
}

// ─── Draw Frames ──────────────────────────────────────────────────────────────

/**
 * @brief Render normal dashboard frame.
 */
static void drawDashboard(SystemState_t state)
{
    display.clearDisplay();

    // ── Header bar ────────────────────────────────────────────────────────
    display.fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(2, 1);
    display.print("GHOSTBOARD  v1.0");
    display.setTextColor(SSD1306_WHITE);

    // ── System state row ──────────────────────────────────────────────────
    display.setCursor(2, 14);
    display.print("STATE: ");
    drawStateIcon(44, 13, state);
    display.setCursor(56, 14);
    display.print(stateLabel(state));

    // ── Task / fault count row ────────────────────────────────────────────
    display.setCursor(2, 25);
    display.print("TASKS: 6/6  FLT:");
    display.print(s_faultCount);

    // ── Temperature row ───────────────────────────────────────────────────
    display.setCursor(2, 36);
    display.print("TEMP: ");
    if (s_lastTemp > -50.0f) {
        display.print(s_lastTemp, 1);
        display.print((char)247); // degree symbol
        display.print("C");
    } else {
        display.print("--.-C");
    }

    // ── Heap usage bar ────────────────────────────────────────────────────
    uint32_t freeHeap  = esp_get_free_heap_size();
    uint32_t totalHeap = 327680; // ESP32 typical total heap
    uint8_t  usedPct   = (uint8_t)(100 - (freeHeap * 100UL / totalHeap));
    uint8_t  barWidth  = (uint8_t)((usedPct * 80UL) / 100);

    display.setCursor(2, 47);
    display.print("MEM:");
    display.drawRect(30, 47, 82, 7, SSD1306_WHITE);
    display.fillRect(31, 48, barWidth, 5, SSD1306_WHITE);

    display.setCursor(114, 47);
    display.print(usedPct);
    display.print("%");

    // ── Recovery dots animation (bottom row) ─────────────────────────────
    if (state == STATE_RECOVERING) {
        display.setCursor(2, 57);
        display.print("RECOVERING");
        for (uint8_t i = 0; i < 4; i++) {
            if (i == s_scrollDot) display.print(".");
            else                  display.print(" ");
        }
    } else if (state == STATE_CRITICAL) {
        display.setCursor(2, 57);
        if (s_blinkPhase) {
            display.print("!! SYSTEM CRITICAL !!");
        }
    }

    display.display();
}

/**
 * @brief Full-screen invert flash for CRITICAL state (high-urgency effect).
 */
static void drawCriticalFlash(void)
{
    display.invertDisplay(s_blinkPhase == 1);
}

// ─── Private: Pull latest sensor temp from queue ──────────────────────────────

static void refreshSensorReading(void)
{
    SensorData_t data;
    // Peek without consuming
    if (xQueuePeek(g_sensorDataQueue, &data, 0) == pdTRUE) {
        if (data.data_valid) {
            s_lastTemp = data.temperature;
        }
    }
}

// ─── Task Entry Point ────────────────────────────────────────────────────────

void oledUITask(void* pvParameters)
{
    // Initialize I2C and OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        GB_LOG(TASK_OLED, LOG_ERROR, "SSD1306 not found — OLED task exiting");
        vTaskDelete(NULL);
        return;
    }
    display.clearDisplay();
    display.display();

    GB_LOG(TASK_OLED, LOG_INFO, "OledUITask online");

    // Boot splash
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("GHOSTBOARD");
    display.setTextSize(1);
    display.setCursor(30, 48);
    display.println("Self-Healing OS");
    display.display();
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t s_prevFaultEvents = 0;

    for (;;) {
        // ── Read current state (mutex-protected) ──────────────────────────
        SystemState_t state;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            state = g_systemState;
            xSemaphoreGive(g_stateMutex);
        } else {
            state = STATE_NERVOUS; // safe default if mutex unavailable
        }

        // ── Update fault counter ──────────────────────────────────────────
        EventBits_t bits = xEventGroupGetBits(g_healthEvents);
        uint8_t faults = __builtin_popcount(bits & 0x3F); // bits 0-5
        if (faults > 0) s_faultCount++;

        // ── Refresh sensor data ───────────────────────────────────────────
        refreshSensorReading();

        // ── Animation tick ────────────────────────────────────────────────
        s_blinkPhase = (s_blinkPhase + 1) % 2;
        s_scrollDot  = (s_scrollDot  + 1) % 4;

        // ── Draw appropriate frame ────────────────────────────────────────
        if (state == STATE_CRITICAL) {
            drawCriticalFlash();
        } else {
            display.invertDisplay(false); // ensure not stuck inverted
        }
        drawDashboard(state);

        // ── Heartbeat ─────────────────────────────────────────────────────
        HEARTBEAT(TASK_OLED);

        // UI refresh rate: 250 ms (fast enough for animations)
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
