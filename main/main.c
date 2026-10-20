#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "system_init.h"
#include "tca9534.h"
#include "lcd.h"
#include "gt911_touch.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "wifi_ui.h"
#include "weather_service.h"
#include "mqtt_service.h"
#include "ota_service.h"

#include "freertos/semphr.h"

static const char *TAG = "MAIN_IOT";

// LVGL Thread Safety Mutex
static SemaphoreHandle_t s_lvgl_mutex = NULL;

bool lvgl_port_lock(uint32_t timeout_ms) {
    if (!s_lvgl_mutex) return true;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE);
}

void lvgl_port_unlock(void) {
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

// -----------------------------------------------------------------------------
// LVGL Tick Callback (2ms Periodic Hardware Timer)
// -----------------------------------------------------------------------------
static void lv_tick_task(void *arg) {
    lv_tick_inc(2); // 2ms increment
}

// -----------------------------------------------------------------------------
// GUI Execution Task (Pinned to Core 1, Priority 5)
// -----------------------------------------------------------------------------
static void gui_task(void *pvParameter) {
    ESP_LOGI(TAG, "GUI Task started on Core %d", xPortGetCoreID());

    uint32_t stats_timer = 0;

    while (1) {
        uint32_t delay_ms = 10;

        // Run LVGL rendering and input processing with thread-safe lock
        if (lvgl_port_lock(100)) {
            delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }

        if (delay_ms < 10) delay_ms = 10;
        if (delay_ms > 30) delay_ms = 30;

        // Educational RTOS Stack Monitoring
        stats_timer += delay_ms;
        if (stats_timer >= 10000) {
            stats_timer = 0;
            UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[RTOS Metrics] gui_task Free Stack: %u words (%u bytes)",
                     (unsigned)stack_remaining, (unsigned)(stack_remaining * sizeof(StackType_t)));
        }

        // Guarantee at least 1 FreeRTOS tick (10ms) delay so CPU 1 IDLE task can run
        TickType_t ticks = pdMS_TO_TICKS(delay_ms);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
    }
}

// -----------------------------------------------------------------------------
// Main Application Entry Point
// -----------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   ESP32 IOT WI-FI & SNTP DASHBOARD INITIALIZING   ");
    ESP_LOGI(TAG, "==================================================");

    // 1. Initialize Board Buses (SPI for ST7796S LCD, I2C for TCA9534 & GT911)
    if (System_Init() != ESP_OK) {
        ESP_LOGE(TAG, "System Bus Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "System Buses Initialized.");
    }

    // 2. Initialize TCA9534 I/O Expander (LCD Backlight, Power, Reset, Touch Reset)
    if (TCA9534_Init() != ESP_OK) {
        ESP_LOGE(TAG, "TCA9534 Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "TCA9534 Initialized.");
    }

    // 3. Initialize ST7796S Display & LVGL Graphic Engine
    if (lcd_lvgl_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "LCD Initialized.");
    }

    // 4. Initialize GT911 Capacitive Touch Controller
    if (gt911_init() != ESP_OK) {
        ESP_LOGE(TAG, "GT911 Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "GT911 Touch Initialized.");
    }

    // 5. Create Recursive Mutex for LVGL Thread Safety
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();

    // 6. Start Hardware Periodic Timer for LVGL Tick (2 ms)
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui_tick"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 2000)); // 2000 us = 2 ms

    // 7. Initialize Wi-Fi UI Screens BEFORE spawning gui_task
    // (Eliminates race condition during widget tree construction)
    if (wifi_ui_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi UI Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "Wi-Fi UI Initialized.");
    }

    // 8. Spawn GUI Task on Core 1 (APP CPU) with Priority 5 and 8KB Stack
    if (xTaskCreatePinnedToCore(gui_task, "gui_task", 8192, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn gui_task!");
    } else {
        ESP_LOGI(TAG, "gui_task spawned on Core 1.");
    }

    // 9. Initialize Wi-Fi Subsystem (NVS, Netif, Event Loop, Wi-Fi Station)
    if (wifi_manager_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi Manager Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "Wi-Fi Manager Initialized.");
    }

    // 10. Initialize Live Weather Service (Core 0 Background Task)
    if (weather_service_init() != ESP_OK) {
        ESP_LOGE(TAG, "Weather Service Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "Weather Service Initialized.");
    }

    // 11. Initialize Real-Time MQTT Telemetry & Remote Control (Core 0)
    if (mqtt_service_init() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT Service Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "MQTT Service Initialized.");
    }

    // 12. Initialize Dual-Partition OTA & Rollback Protection
    if (ota_service_init() != ESP_OK) {
        ESP_LOGE(TAG, "OTA Service Initialization Failed!");
    } else {
        ESP_LOGI(TAG, "OTA Service Initialized.");
    }

    // 13. Check if saved Wi-Fi credentials exist in Flash NVS
    char saved_ssid[WIFI_MAX_SSID_LEN] = {0};
    char saved_pwd[WIFI_MAX_PASSWORD_LEN] = {0};
    if (wifi_manager_load_credentials(saved_ssid, saved_pwd) == ESP_OK && strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Found stored Wi-Fi credentials in Flash NVS for: [%s]", saved_ssid);
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "Auto-connecting to [%s]...", saved_ssid);
        wifi_ui_set_status(status_msg, lv_color_hex(0x38BDF8));
        wifi_ui_show_spinner(true);
        wifi_manager_connect(saved_ssid, saved_pwd);
    } else {
        ESP_LOGI(TAG, "No stored credentials found. Starting initial Wi-Fi scan...");
        wifi_ui_set_status("Scanning for Wi-Fi networks...", lv_color_hex(0xFBBF24));
        wifi_ui_show_spinner(true);
        wifi_manager_start_scan();
    }

    ESP_LOGI(TAG, "System bringup complete. Ready for user interaction.");
}

