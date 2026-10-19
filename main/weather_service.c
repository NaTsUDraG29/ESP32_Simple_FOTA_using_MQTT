#include "weather_service.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WEATHER_SVC";

#define WEATHER_HTTP_BUFFER_SIZE    2048
#define WEATHER_REFRESH_INTERVAL_MS (15 * 60 * 1000) // 15 minutes

// Location Coordinates (Default: Bengaluru, India)
static float s_latitude = 12.9716f;
static float s_longitude = 77.5946f;
static char  s_city_name[32] = "Bengaluru";

// Internal State
static weather_data_t       s_weather_data = {0};
static SemaphoreHandle_t    s_weather_mutex = NULL;
static TaskHandle_t         s_weather_task_handle = NULL;
static weather_update_cb_t  s_update_cb = NULL;
static void                *s_update_cb_user_data = NULL;

// HTTP Response Accumulation Buffer
static char  s_http_rx_buf[WEATHER_HTTP_BUFFER_SIZE];
static int   s_http_rx_len = 0;

// -----------------------------------------------------------------------------
// WMO Weather Code Decoder
// -----------------------------------------------------------------------------
static void decode_wmo_code(int code, char *desc_out, size_t max_len) {
    const char *desc = "Unknown";
    switch (code) {
        case 0:  desc = "Clear Sky"; break;
        case 1:  desc = "Mainly Clear"; break;
        case 2:  desc = "Partly Cloudy"; break;
        case 3:  desc = "Overcast"; break;
        case 45:
        case 48: desc = "Foggy"; break;
        case 51:
        case 53:
        case 55: desc = "Drizzle"; break;
        case 61: desc = "Light Rain"; break;
        case 63: desc = "Moderate Rain"; break;
        case 65: desc = "Heavy Rain"; break;
        case 71:
        case 73:
        case 75: desc = "Snow"; break;
        case 77: desc = "Snow Grains"; break;
        case 80:
        case 81:
        case 82: desc = "Rain Showers"; break;
        case 85:
        case 86: desc = "Snow Showers"; break;
        case 95: desc = "Thunderstorm"; break;
        case 96:
        case 99: desc = "Thunderstorm w/ Hail"; break;
        default: desc = "Partly Cloudy"; break;
    }
    strncpy(desc_out, desc, max_len - 1);
    desc_out[max_len - 1] = '\0';
}

// -----------------------------------------------------------------------------
// HTTP Client Event Handler
// -----------------------------------------------------------------------------
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            s_http_rx_len = 0;
            s_http_rx_buf[0] = '\0';
            break;

        case HTTP_EVENT_ON_DATA:
            if (s_http_rx_len + evt->data_len < WEATHER_HTTP_BUFFER_SIZE - 1) {
                memcpy(s_http_rx_buf + s_http_rx_len, evt->data, evt->data_len);
                s_http_rx_len += evt->data_len;
                s_http_rx_buf[s_http_rx_len] = '\0';
            } else {
                ESP_LOGW(TAG, "HTTP RX buffer overflow!");
            }
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP Event Error occurred.");
            break;

        default:
            break;
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Fetch & Parse Weather from Open-Meteo REST API
// -----------------------------------------------------------------------------
static esp_err_t fetch_weather_data(void) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m",
             s_latitude, s_longitude);

    ESP_LOGI(TAG, "Fetching live weather from Open-Meteo for [%s]...", s_city_name);
    s_http_rx_len = 0;
    s_http_rx_buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach, // Mozilla Root CA verification
        .timeout_ms = 12000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client!");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed! err: %s, HTTP Status: %d", esp_err_to_name(err), status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received %d bytes from weather API. Parsing JSON...", s_http_rx_len);

    // Parse JSON
    cJSON *root = cJSON_Parse(s_http_rx_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON body!");
        return ESP_FAIL;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!current) {
        ESP_LOGE(TAG, "JSON missing 'current' object!");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *temp_item = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *hum_item  = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *wind_item = cJSON_GetObjectItem(current, "wind_speed_10m");
    cJSON *code_item = cJSON_GetObjectItem(current, "weather_code");

    if (!temp_item || !hum_item || !wind_item || !code_item) {
        ESP_LOGE(TAG, "JSON missing weather field values!");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // Protect state update with mutex
    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_weather_data.temperature = (float)temp_item->valuedouble;
        s_weather_data.humidity = hum_item->valueint;
        s_weather_data.wind_speed = (float)wind_item->valuedouble;
        s_weather_data.weather_code = code_item->valueint;
        decode_wmo_code(s_weather_data.weather_code, s_weather_data.condition, sizeof(s_weather_data.condition));
        strncpy(s_weather_data.city, s_city_name, sizeof(s_weather_data.city) - 1);
        s_weather_data.valid = true;
        s_weather_data.last_update_sec = (uint32_t)time(NULL);

        xSemaphoreGive(s_weather_mutex);

        ESP_LOGI(TAG, ">>> Weather Updated: [%s] %.1f°C | %s | %d%% Humidity | Wind: %.1f km/h <<<",
                 s_weather_data.city, s_weather_data.temperature,
                 s_weather_data.condition, s_weather_data.humidity,
                 s_weather_data.wind_speed);

        // Notify registered subscriber (e.g. UI)
        if (s_update_cb) {
            s_update_cb(&s_weather_data, s_update_cb_user_data);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// FreeRTOS Weather Background Task (Core 0, Priority 4)
// -----------------------------------------------------------------------------
static void weather_task(void *pvParameters) {
    ESP_LOGI(TAG, "Weather background task running on Core %d", xPortGetCoreID());

    // Give Wi-Fi and SNTP 5 seconds to settle on initial boot
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        // HTTPS TLS certificate verification requires an accurate system clock!
        if (wifi_manager_get_state() == WIFI_STATE_CONNECTED && wifi_manager_is_time_synced()) {
            fetch_weather_data();

            // Wait 15 minutes or until an immediate refresh notification is sent
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS));
        } else {
            // Not connected or SNTP still synchronizing; check again in 3 seconds
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

// -----------------------------------------------------------------------------
// Public Functions
// -----------------------------------------------------------------------------
esp_err_t weather_service_init(void) {
    s_weather_mutex = xSemaphoreCreateMutex();
    if (!s_weather_mutex) {
        ESP_LOGE(TAG, "Failed to create weather mutex!");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        weather_task,
        "weather_task",
        6144, // 6 KB stack for TLS handshake and JSON parsing
        NULL,
        4,    // Priority 4 (below Wi-Fi task, same as app_main)
        &s_weather_task_handle,
        0     // Core 0 (Networking Core)
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn weather_task!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Weather service initialized.");
    return ESP_OK;
}

void weather_service_register_cb(weather_update_cb_t cb, void *user_data) {
    s_update_cb = cb;
    s_update_cb_user_data = user_data;
}

void weather_service_trigger_refresh(void) {
    if (s_weather_task_handle) {
        xTaskNotifyGive(s_weather_task_handle);
    }
}

bool weather_service_get_data(weather_data_t *out_data) {
    if (!out_data || !s_weather_mutex) return false;
    bool valid = false;
    if (xSemaphoreTake(s_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(out_data, &s_weather_data, sizeof(weather_data_t));
        valid = s_weather_data.valid;
        xSemaphoreGive(s_weather_mutex);
    }
    return valid;
}

void weather_service_set_location(float lat, float lon, const char *city_name) {
    s_latitude = lat;
    s_longitude = lon;
    if (city_name) {
        strncpy(s_city_name, city_name, sizeof(s_city_name) - 1);
        s_city_name[sizeof(s_city_name) - 1] = '\0';
    }
    weather_service_trigger_refresh();
}
