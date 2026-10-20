#include "mqtt_service.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

#include "wifi_manager.h"
#include "wifi_ui.h"
#include "weather_service.h"
#include "ota_service.h"

static const char *TAG = "MQTT_SERVICE";

// -----------------------------------------------------------------------------
// State & Handles
// -----------------------------------------------------------------------------
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool                     s_is_connected = false;
static bool                     s_client_started = false;

// Callbacks
static mqtt_status_cb_t         s_status_cb = NULL;
static void                    *s_status_cb_user_data = NULL;

// Topics and Identifiers
static char s_client_id[32]        = {0};
static char s_topic_telemetry[MQTT_MAX_TOPIC_LEN] = {0};
static char s_topic_cmd[MQTT_MAX_TOPIC_LEN]       = {0};
static char s_topic_status[MQTT_MAX_TOPIC_LEN]    = {0};
static char s_topic_response[MQTT_MAX_TOPIC_LEN]  = {0};
static const char *s_topic_broadcast_cmd          = "esp32_iot/all/cmd";

// Telemetry Task Synchronization
static TaskHandle_t s_telem_task_handle = NULL;

// Forward Declarations
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void handle_mqtt_command(const char *topic, int topic_len, const char *data, int data_len);
static void mqtt_telemetry_task(void *pvParameters);
static void on_wifi_state_changed(wifi_state_t new_state, void *user_data);

// -----------------------------------------------------------------------------
// Public Functions
// -----------------------------------------------------------------------------
esp_err_t mqtt_service_init(void) {
    ESP_LOGI(TAG, "Initializing Real-Time MQTT Cloud Telemetry & Control Service...");

    // 1. Derive unique Client ID and Topics from Factory MAC Address
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id), "esp32_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "esp32_iot/%s/telemetry", s_client_id);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "esp32_iot/%s/cmd", s_client_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "esp32_iot/%s/status", s_client_id);
    snprintf(s_topic_response, sizeof(s_topic_response), "esp32_iot/%s/response", s_client_id);

    ESP_LOGI(TAG, "================================================================");
    ESP_LOGI(TAG, "MQTT Configured:");
    ESP_LOGI(TAG, "  Broker:      %s", MQTT_DEFAULT_BROKER_URI);
    ESP_LOGI(TAG, "  Client ID:   %s", s_client_id);
    ESP_LOGI(TAG, "  Telemetry:   %s", s_topic_telemetry);
    ESP_LOGI(TAG, "  Command:     %s", s_topic_cmd);
    ESP_LOGI(TAG, "  Broadcast:   %s", s_topic_broadcast_cmd);
    ESP_LOGI(TAG, "  Web Client:  http://www.emqx.io/online-mqtt-client/");
    ESP_LOGI(TAG, "================================================================");

    // 2. Configure MQTT Client (MQTT 3.1.1, TCP Port 1883 with LWT)
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_DEFAULT_BROKER_URI,
        },
        .credentials = {
            .client_id = s_client_id,
        },
        .session = {
            .last_will = {
                .topic = s_topic_status,
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = 1,
            },
            .keepalive = 60,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client!");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    // 3. Register for Wi-Fi State Changes to auto-start/stop MQTT
    wifi_manager_register_state_cb(on_wifi_state_changed, NULL);

    // 4. Spawn Background Telemetry Task on Core 0 (Priority 4, 4KB Stack)
    BaseType_t ret = xTaskCreatePinnedToCore(
        mqtt_telemetry_task,
        "mqtt_telem",
        4096,
        NULL,
        4,
        &s_telem_task_handle,
        0 // Core 0 (PRO CPU)
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn mqtt_telemetry_task!");
        return ESP_FAIL;
    }

    // 5. If Wi-Fi is already connected, start the client immediately
    if (wifi_manager_get_state() == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi is already connected. Starting MQTT client...");
        esp_mqtt_client_start(s_mqtt_client);
        s_client_started = true;
    }

    return ESP_OK;
}

void mqtt_service_register_status_cb(mqtt_status_cb_t cb, void *user_data) {
    s_status_cb = cb;
    s_status_cb_user_data = user_data;
}

bool mqtt_service_is_connected(void) {
    return s_is_connected;
}

esp_err_t mqtt_service_publish(const char *topic, const char *data, int qos, int retain) {
    if (!s_mqtt_client || !s_is_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, 0, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

void mqtt_service_trigger_telemetry(void) {
    if (s_telem_task_handle) {
        xTaskNotifyGive(s_telem_task_handle);
    }
}

const char* mqtt_service_get_client_id(void) {
    return s_client_id;
}

const char* mqtt_service_get_telemetry_topic(void) {
    return s_topic_telemetry;
}

const char* mqtt_service_get_cmd_topic(void) {
    return s_topic_cmd;
}

// -----------------------------------------------------------------------------
// Wi-Fi State Observer (Connect / Disconnect Lifecycle)
// -----------------------------------------------------------------------------
static void on_wifi_state_changed(wifi_state_t new_state, void *user_data) {
    if (new_state == WIFI_STATE_CONNECTED) {
        if (s_mqtt_client && !s_client_started) {
            ESP_LOGI(TAG, "Wi-Fi connected -> Starting MQTT client...");
            esp_mqtt_client_start(s_mqtt_client);
            s_client_started = true;
        }
    } else if (new_state == WIFI_STATE_DISCONNECTED || new_state == WIFI_STATE_CONNECT_FAILED) {
        if (s_mqtt_client && s_client_started) {
            ESP_LOGW(TAG, "Wi-Fi disconnected -> Stopping MQTT client...");
            esp_mqtt_client_stop(s_mqtt_client);
            s_client_started = false;
            s_is_connected = false;
            wifi_ui_set_mqtt_status(false);
            if (s_status_cb) {
                s_status_cb(false, s_status_cb_user_data);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// MQTT Event Handler (Runs in esp-mqtt task context on Core 0)
// -----------------------------------------------------------------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_is_connected = true;
            ESP_LOGI(TAG, "--------------------------------------------------------");
            ESP_LOGI(TAG, ">>> MQTT BROKER CONNECTED SUCCESSFULLY! <<<");
            ESP_LOGI(TAG, "Subscribing to device command topic: %s", s_topic_cmd);
            esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 1);
            ESP_LOGI(TAG, "Subscribing to broadcast command topic: %s", s_topic_broadcast_cmd);
            esp_mqtt_client_subscribe(s_mqtt_client, s_topic_broadcast_cmd, 1);

            // Publish retained LWT status: "online"
            esp_mqtt_client_publish(s_mqtt_client, s_topic_status, "online", 0, 1, 1);

            // Notify UI
            wifi_ui_set_mqtt_status(true);
            if (s_status_cb) {
                s_status_cb(true, s_status_cb_user_data);
            }

            // Immediately trigger telemetry broadcast
            mqtt_service_trigger_telemetry();
            ESP_LOGI(TAG, "--------------------------------------------------------");
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_is_connected = false;
            ESP_LOGW(TAG, "MQTT broker disconnected.");
            wifi_ui_set_mqtt_status(false);
            if (s_status_cb) {
                s_status_cb(false, s_status_cb_user_data);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscription acknowledged (msg_id=%d)", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT message received on topic: %.*s", event->topic_len, event->topic);
            handle_mqtt_command(event->topic, event->topic_len, event->data, event->data_len);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error event encountered (type=%d)", event->error_handle->error_type);
            break;

        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Inbound JSON Command Dispatcher
// -----------------------------------------------------------------------------
static void handle_mqtt_command(const char *topic, int topic_len, const char *data, int data_len) {
    if (!data || data_len <= 0) return;

    // Parse JSON safely using explicit length
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse inbound MQTT payload as JSON!");
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring) {
        ESP_LOGW(TAG, "Missing 'cmd' string in inbound JSON payload.");
        cJSON_Delete(root);
        return;
    }

    const char *cmd = cmd_item->valuestring;
    ESP_LOGI(TAG, "Executing Remote MQTT Command: [%s]", cmd);

    // Prepare JSON Response
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "device", s_client_id);
    cJSON_AddStringToObject(resp, "cmd", cmd);

    if (strcmp(cmd, "display") == 0) {
        cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (cJSON_IsString(action_item) && action_item->valuestring) {
            if (strcmp(action_item->valuestring, "sleep") == 0) {
                wifi_ui_sleep_display();
                cJSON_AddStringToObject(resp, "status", "ok");
                cJSON_AddStringToObject(resp, "display", "sleeping");
            } else if (strcmp(action_item->valuestring, "wake") == 0) {
                wifi_ui_wake_display();
                cJSON_AddStringToObject(resp, "status", "ok");
                cJSON_AddStringToObject(resp, "display", "awake");
            } else {
                cJSON_AddStringToObject(resp, "status", "error");
                cJSON_AddStringToObject(resp, "error", "unknown_action");
            }
        }
    } else if (strcmp(cmd, "toast") == 0) {
        cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(root, "msg");
        cJSON *dur_item = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
        uint32_t duration = (cJSON_IsNumber(dur_item) && dur_item->valueint > 0) ? dur_item->valueint : 4000;

        if (cJSON_IsString(msg_item) && msg_item->valuestring) {
            wifi_ui_show_toast(msg_item->valuestring, duration);
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "toast_shown", msg_item->valuestring);
        } else {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "missing_msg");
        }
    } else if (strcmp(cmd, "set_city") == 0) {
        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
        cJSON *lat_item  = cJSON_GetObjectItemCaseSensitive(root, "lat");
        cJSON *lon_item  = cJSON_GetObjectItemCaseSensitive(root, "lon");

        if (cJSON_IsString(name_item) && cJSON_IsNumber(lat_item) && cJSON_IsNumber(lon_item)) {
            weather_service_set_location((float)lat_item->valuedouble, (float)lon_item->valuedouble, name_item->valuestring);
            weather_service_trigger_refresh();
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "city", name_item->valuestring);
        } else {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "invalid_location_params");
        }
    } else if (strcmp(cmd, "ping") == 0) {
        cJSON_AddStringToObject(resp, "status", "pong");
        cJSON_AddNumberToObject(resp, "uptime_sec", (int)(esp_timer_get_time() / 1000000));
        mqtt_service_trigger_telemetry();
    } else if (strcmp(cmd, "ota") == 0) {
        cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring) {
            ESP_LOGI(TAG, "OTA Trigger requested via MQTT: %s", url_item->valuestring);
            esp_err_t ota_err = ota_service_start(url_item->valuestring);
            if (ota_err == ESP_OK) {
                cJSON_AddStringToObject(resp, "status", "started");
                cJSON_AddStringToObject(resp, "ota_url", url_item->valuestring);
            } else {
                cJSON_AddStringToObject(resp, "status", "error");
                cJSON_AddStringToObject(resp, "error", esp_err_to_name(ota_err));
            }
        } else {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "missing_ota_url");
        }
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "unknown_command");
    }

    // Publish Response JSON
    char *resp_str = cJSON_PrintUnformatted(resp);
    if (resp_str) {
        mqtt_service_publish(s_topic_response, resp_str, 1, 0);
        cJSON_free(resp_str);
    }

    cJSON_Delete(resp);
    cJSON_Delete(root);
}

// -----------------------------------------------------------------------------
// Core 0 Periodic Telemetry Task
// -----------------------------------------------------------------------------
static void mqtt_telemetry_task(void *pvParameters) {
    ESP_LOGI(TAG, "MQTT Telemetry Task started on Core %d", xPortGetCoreID());

    while (1) {
        // Wait for interval timeout (15s) or direct task notification (e.g., ping)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MQTT_TELEMETRY_INTERVAL_SEC * 1000));

        if (!s_is_connected || !s_mqtt_client) {
            continue;
        }

        // Build Telemetry JSON
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device", "ESP32-WROVER-E");
        cJSON_AddStringToObject(root, "client_id", s_client_id);
        cJSON_AddNumberToObject(root, "uptime_sec", (int)(esp_timer_get_time() / 1000000));
        cJSON_AddNumberToObject(root, "free_heap", (int)esp_get_free_heap_size());
        cJSON_AddNumberToObject(root, "min_free_heap", (int)esp_get_minimum_free_heap_size());
        cJSON_AddStringToObject(root, "display", wifi_ui_is_display_sleeping() ? "sleeping" : "awake");

        // Network Information
        wifi_ip_info_t ip_info;
        if (wifi_manager_get_ip_info(&ip_info)) {
            cJSON *wifi_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(wifi_obj, "ssid", ip_info.ssid);
            cJSON_AddStringToObject(wifi_obj, "ip", ip_info.ip);
            cJSON_AddNumberToObject(wifi_obj, "rssi", ip_info.rssi);
            cJSON_AddItemToObject(root, "wifi", wifi_obj);
        }

        // Weather Information
        weather_data_t weather;
        if (weather_service_get_data(&weather)) {
            cJSON *weather_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(weather_obj, "city", weather.city);
            cJSON_AddNumberToObject(weather_obj, "temp_c", weather.temperature);
            cJSON_AddStringToObject(weather_obj, "condition", weather.condition);
            cJSON_AddNumberToObject(weather_obj, "humidity", weather.humidity);
            cJSON_AddNumberToObject(weather_obj, "wind_kmh", weather.wind_speed);
            cJSON_AddItemToObject(root, "weather", weather_obj);
        }

        char *payload = cJSON_PrintUnformatted(root);
        if (payload) {
            esp_mqtt_client_publish(s_mqtt_client, s_topic_telemetry, payload, 0, 0, 0);
            ESP_LOGI(TAG, "Published Telemetry to [%s]: %s", s_topic_telemetry, payload);
            cJSON_free(payload);
        }

        cJSON_Delete(root);
    }
}

