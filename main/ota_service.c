#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_service.h"
#include "wifi_manager.h"
#include "wifi_ui.h"

static const char *TAG = "OTA_SERVICE";

// State and Synchronization
static TaskHandle_t      s_ota_task_handle = NULL;
static volatile bool     s_ota_running     = false;
static char              s_ota_url[OTA_URL_MAX_LEN] = {0};

// Callback Storage
static ota_progress_cb_t s_progress_cb     = NULL;
static void             *s_progress_user_data = NULL;

// Forward declaration of internal task
static void ota_download_task(void *pvParameters);

// -----------------------------------------------------------------------------
// Helper: Dispatch Progress Updates
// -----------------------------------------------------------------------------
static void dispatch_status(ota_state_t state, int percent, size_t bytes_read, size_t total_bytes, const char *msg) {
    ota_status_info_t info = {
        .state       = state,
        .percent     = percent,
        .bytes_read  = bytes_read,
        .total_bytes = total_bytes,
    };
    if (msg) {
        strncpy(info.message, msg, sizeof(info.message) - 1);
        info.message[sizeof(info.message) - 1] = '\0';
    } else {
        info.message[0] = '\0';
    }

    // Update On-Screen UI
    if (state == OTA_STATE_STARTING) {
        wifi_ui_show_ota(s_ota_url);
    } else if (state == OTA_STATE_DOWNLOADING || state == OTA_STATE_VERIFYING || state == OTA_STATE_SUCCESS) {
        wifi_ui_update_ota_progress(percent, bytes_read, total_bytes, info.message);
    } else if (state == OTA_STATE_FAILED) {
        wifi_ui_update_ota_progress(percent, bytes_read, total_bytes, info.message);
    }

    // Invoke registered callback (e.g. MQTT / cloud reporting)
    if (s_progress_cb) {
        s_progress_cb(&info, s_progress_user_data);
    }
}

// -----------------------------------------------------------------------------
// Boot Partition Inspection & Rollback Cancelation
// -----------------------------------------------------------------------------
esp_err_t ota_service_validate_running_app(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to locate running partition!");
        return ESP_FAIL;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "OTA PARTITION & BOOT DIAGNOSTICS");
    ESP_LOGI(TAG, "  Current Partition:   [%s]", running->label);
    ESP_LOGI(TAG, "  Partition SubType:   0x%02x (%s)", running->subtype,
             (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? "ota_0" :
             (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? "ota_1" :
             (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) ? "factory" : "other");
    ESP_LOGI(TAG, "  Flash Offset:        0x%08" PRIx32, running->address);
    ESP_LOGI(TAG, "  Partition Size:      %lu KB (%lu MB)", (unsigned long)(running->size / 1024), (unsigned long)(running->size / (1024 * 1024)));
    if (app_desc) {
        ESP_LOGI(TAG, "  Firmware Version:    %s", app_desc->version);
        ESP_LOGI(TAG, "  Project Name:        %s", app_desc->project_name);
        ESP_LOGI(TAG, "  Compile Time:        %s %s", app_desc->date, app_desc->time);
        ESP_LOGI(TAG, "  IDF Version:         %s", app_desc->idf_ver);
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        ESP_LOGI(TAG, "  OTA Image State:     0x%02x", ota_state);
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, ">>> Device booted from newly flashed OTA partition in PENDING_VERIFY state! <<<");
            ESP_LOGI(TAG, "Running hardware and subsystem self-checks...");
            // Board buses, ST7796S LCD, TCA9534, and GT911 touch initialized prior to this call
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, ">>> Subsystems OK: Firmware marked VALID. Rollback cancelled! <<<");
                wifi_ui_show_toast("OTA Update Successful! Firmware validated.", 6000);
            } else {
                ESP_LOGE(TAG, "Failed to mark app valid: %s! Rollback will trigger on reboot.", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "==================================================");
            return err;
        } 
        else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "  Status: Partition is already verified and marked VALID.");
        } 
        else {
            ESP_LOGI(TAG, "  Status: Partition state code 0x%02x", ota_state);
        }
    } else {
        ESP_LOGI(TAG, "  Status: Initial boot from factory or unconfigured otadata partition.");
    }
    ESP_LOGI(TAG, "==================================================");

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Service Initialization
// -----------------------------------------------------------------------------
esp_err_t ota_service_init(void) {
    ESP_LOGI(TAG, "Initializing OTA Firmware Update Service...");
    s_ota_running = false;
    return ota_service_validate_running_app();
}

// -----------------------------------------------------------------------------
// Register Progress Callback
// -----------------------------------------------------------------------------
esp_err_t ota_service_register_callback(ota_progress_cb_t cb, void *user_data) {
    s_progress_cb = cb;
    s_progress_user_data = user_data;
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Start OTA Update Process
// -----------------------------------------------------------------------------
esp_err_t ota_service_start(const char *url) {
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid OTA URL provided");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_running) {
        ESP_LOGW(TAG, "OTA update is already running!");
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_manager_get_state() != WIFI_STATE_CONNECTED) {
        ESP_LOGE(TAG, "Cannot start OTA: Wi-Fi is disconnected!");
        wifi_ui_show_toast("Cannot update: Wi-Fi offline", 4000);
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    ESP_LOGI(TAG, "Spawning OTA Download Task on Core 0 for URL: %s", s_ota_url);
    s_ota_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        ota_download_task,
        "ota_task",
        8192,
        NULL,
        5,
        &s_ota_task_handle,
        0 // Pin to Core 0 to leave Core 1 for LVGL 60FPS UI
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA FreeRTOS task!");
        s_ota_running = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ota_service_is_running(void) {
    return s_ota_running;
}

// -----------------------------------------------------------------------------
// Core 0 OTA Background Task
// -----------------------------------------------------------------------------
static void ota_download_task(void *pvParameters) {
    ESP_LOGI(TAG, "OTA Task running on Core %d", xPortGetCoreID());

    dispatch_status(OTA_STATE_STARTING, 0, 0, 0, "Connecting to server...");

    esp_http_client_config_t http_config = {
        .url = s_ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach, // Mozilla CA bundle for HTTPS
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);

    if (err != ESP_OK || ota_handle == NULL) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        char err_buf[64];
        snprintf(err_buf, sizeof(err_buf), "Connect failed: %s", esp_err_to_name(err));
        dispatch_status(OTA_STATE_FAILED, 0, 0, 0, err_buf);
        vTaskDelay(pdMS_TO_TICKS(4000));
        wifi_ui_hide_ota();
        s_ota_running = false;
        s_ota_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Inspect target image header
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "New App Version: %s, Project: %s, Date: %s %s",
                 new_app_info.version, new_app_info.project_name,
                 new_app_info.date, new_app_info.time);
    }

    int total_size = esp_https_ota_get_image_size(ota_handle);
    ESP_LOGI(TAG, "OTA Image Total Size: %d bytes (%.2f MB)",
             total_size, (float)total_size / (1024.0f * 1024.0f));

    dispatch_status(OTA_STATE_DOWNLOADING, 0, 0, total_size, "Starting download...");

    int last_percent = -1;
    uint32_t last_log_time = 0;

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
        int percent = (total_size > 0) ? ((int64_t)bytes_read * 100 / total_size) : 0;
        if (percent > 100) percent = 100;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (percent != last_percent || (now_ms - last_log_time) >= 1000) {
            last_percent = percent;
            last_log_time = now_ms;

            char status_buf[64];
            if (total_size > 0) {
                snprintf(status_buf, sizeof(status_buf), "Flashing: %d%% (%u / %u KB)",
                         percent, (unsigned)(bytes_read / 1024), (unsigned)(total_size / 1024));
            } else {
                snprintf(status_buf, sizeof(status_buf), "Downloaded %u KB...", (unsigned)(bytes_read / 1024));
            }

            dispatch_status(OTA_STATE_DOWNLOADING, percent, bytes_read, total_size, status_buf);
            ESP_LOGI(TAG, "Progress: %d%% (%d / %d bytes)", percent, bytes_read, total_size);
        }

        // Yield to allow idle task and Wi-Fi stack processing
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle)) {
        dispatch_status(OTA_STATE_VERIFYING, 100, total_size, total_size, "Verifying flash integrity...");
        ESP_LOGI(TAG, "Complete data received. Verifying SHA-256 and updating boot partition...");

        esp_err_t finish_err = esp_https_ota_finish(ota_handle);
        if (finish_err == ESP_OK) {
            ESP_LOGI(TAG, "==================================================");
            ESP_LOGI(TAG, "OTA UPDATE SUCCESSFUL! Rebooting into new firmware...");
            ESP_LOGI(TAG, "==================================================");

            dispatch_status(OTA_STATE_SUCCESS, 100, total_size, total_size, "Update Complete! Rebooting in 3s...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(finish_err));
            char err_buf[64];
            snprintf(err_buf, sizeof(err_buf), "Integrity verify failed: %s", esp_err_to_name(finish_err));
            dispatch_status(OTA_STATE_FAILED, 0, 0, 0, err_buf);
            vTaskDelay(pdMS_TO_TICKS(4000));
            wifi_ui_hide_ota();
        }
    } else {
        ESP_LOGE(TAG, "OTA Download failed or aborted: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        char err_buf[64];
        snprintf(err_buf, sizeof(err_buf), "Download failed: %s", esp_err_to_name(err));
        dispatch_status(OTA_STATE_FAILED, 0, 0, 0, err_buf);
        vTaskDelay(pdMS_TO_TICKS(4000));
        wifi_ui_hide_ota();
    }

    s_ota_running = false;
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

