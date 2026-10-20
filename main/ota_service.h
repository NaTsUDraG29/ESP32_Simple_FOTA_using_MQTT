#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_URL_MAX_LEN 256

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_STARTING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    ota_state_t state;
    int percent;            // 0 - 100, or -1 if unknown
    size_t bytes_read;
    size_t total_bytes;
    char message[96];
} ota_status_info_t;

// Callback for OTA progress and state notifications
typedef void (*ota_progress_cb_t)(const ota_status_info_t *info, void *user_data);

/**
 * @brief Initialize OTA service and inspect/validate boot partition.
 *        If partition is in PENDING_VERIFY state, marks it VALID to prevent rollback.
 */
esp_err_t ota_service_init(void);

/**
 * @brief Register a callback for OTA progress events (UI, MQTT, logging).
 */
esp_err_t ota_service_register_callback(ota_progress_cb_t cb, void *user_data);

/**
 * @brief Start OTA firmware download from HTTP or HTTPS URL in background task on Core 0.
 *
 * @param url URL to the firmware binary (e.g. http://192.168.1.100:8000/firmware.bin)
 * @return ESP_OK on task start, ESP_ERR_INVALID_STATE if already running, ESP_ERR_WIFI_NOT_CONNECT if offline.
 */
esp_err_t ota_service_start(const char *url);

/**
 * @brief Check if an OTA update is actively in progress.
 */
bool ota_service_is_running(void);

/**
 * @brief Mark current running app partition as valid to cancel bootloader rollback.
 */
esp_err_t ota_service_validate_running_app(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_SERVICE_H

