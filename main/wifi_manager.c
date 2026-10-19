#include "wifi_manager.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI_MGR";

// Event Group Bits
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_SCAN_DONE_BIT      BIT2

// NVS Namespace & Keys
#define NVS_WIFI_NAMESPACE      "wifi_store"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASSWORD        "pwd"

#define MAX_RETRY_COUNT         5

// Internal State Variables
static EventGroupHandle_t       s_wifi_event_group = NULL;
static esp_netif_t             *s_sta_netif = NULL;
static wifi_state_t             s_current_state = WIFI_STATE_UNINITIALIZED;
static wifi_state_cb_t          s_state_cb = NULL;
static void                    *s_state_cb_user_data = NULL;

static wifi_ui_ap_t             s_scanned_aps[MAX_SCANNED_APS];
static uint16_t                 s_scanned_count = 0;

static wifi_ip_info_t           s_ip_info = {0};
static int                      s_retry_num = 0;
static bool                     s_time_synced = false;
static bool                     s_user_requested_disconnect = false;

// -----------------------------------------------------------------------------
// Helper: Notify State Change
// -----------------------------------------------------------------------------
static void set_state(wifi_state_t new_state) {
    s_current_state = new_state;
    ESP_LOGI(TAG, "Wi-Fi State Transition -> %d", (int)new_state);
    if (s_state_cb) {
        s_state_cb(new_state, s_state_cb_user_data);
    }
}

// -----------------------------------------------------------------------------
// SNTP Time Synchronization Callback
// -----------------------------------------------------------------------------
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Time Synchronized via SNTP!");
    s_time_synced = true;

    // Set Timezone to Indian Standard Time (IST, UTC+5:30)
    // Format: "IST-5:30" (POSIX standard: sign is inverted from UTC offset)
    setenv("TZ", "IST-5:30", 1);
    tzset();

    char strftime_buf[64];
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current Local Time: %s", strftime_buf);
}

// -----------------------------------------------------------------------------
// ESP-IDF Event Handler (Wi-Fi & IP Events)
// -----------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Wi-Fi Station Started.");
                set_state(WIFI_STATE_IDLE);
                break;

            case WIFI_EVENT_SCAN_DONE: {
                uint16_t ap_count = 0;
                esp_wifi_scan_get_ap_num(&ap_count);
                uint16_t num_to_get = (ap_count < MAX_SCANNED_APS) ? ap_count : MAX_SCANNED_APS;

                wifi_ap_record_t *raw_records = malloc(sizeof(wifi_ap_record_t) * num_to_get);
                if (raw_records) {
                    esp_wifi_scan_get_ap_records(&num_to_get, raw_records);

                    s_scanned_count = 0;
                    for (int i = 0; i < num_to_get; i++) {
                        // Skip empty SSIDs (hidden networks)
                        if (strlen((char *)raw_records[i].ssid) == 0) continue;

                        strncpy(s_scanned_aps[s_scanned_count].ssid, (char *)raw_records[i].ssid, WIFI_MAX_SSID_LEN - 1);
                        s_scanned_aps[s_scanned_count].ssid[WIFI_MAX_SSID_LEN - 1] = '\0';
                        s_scanned_aps[s_scanned_count].rssi = raw_records[i].rssi;
                        s_scanned_aps[s_scanned_count].authmode = raw_records[i].authmode;
                        s_scanned_aps[s_scanned_count].is_secured = (raw_records[i].authmode != WIFI_AUTH_OPEN);
                        s_scanned_count++;
                    }
                    free(raw_records);
                }
                ESP_LOGI(TAG, "Scan Complete! Found %d usable APs.", s_scanned_count);
                xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
                set_state(WIFI_STATE_SCAN_DONE); //for LCD
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Associated with AP. Requesting IP address via DHCP...");
                s_retry_num = 0;
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected from AP, reason code: %d", disc->reason);

                if (!s_user_requested_disconnect) {
                    if (s_retry_num < MAX_RETRY_COUNT) {
                        s_retry_num++;
                        ESP_LOGW(TAG, "Wi-Fi link lost / disconnected unexpectedly! Auto-reconnecting (%d/%d)...",
                                 s_retry_num, MAX_RETRY_COUNT);
                        set_state(WIFI_STATE_CONNECTING);
                        vTaskDelay(pdMS_TO_TICKS(1500)); // Small backoff before retrying
                        esp_wifi_connect();
                    } else {
                        ESP_LOGE(TAG, "Auto-reconnect failed after %d retries.", MAX_RETRY_COUNT);
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                        set_state(WIFI_STATE_CONNECT_FAILED);
                    }
                } else {
                    ESP_LOGI(TAG, "User requested disconnect; not reconnecting.");
                    set_state(WIFI_STATE_DISCONNECTED);
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_info.ip, sizeof(s_ip_info.ip), IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_ip_info.netmask, sizeof(s_ip_info.netmask), IPSTR, IP2STR(&event->ip_info.netmask));
            snprintf(s_ip_info.gateway, sizeof(s_ip_info.gateway), IPSTR, IP2STR(&event->ip_info.gw));

            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                strncpy(s_ip_info.ssid, (char *)ap_info.ssid, sizeof(s_ip_info.ssid) - 1);
                s_ip_info.rssi = ap_info.rssi;
            }

            ESP_LOGI(TAG, "IP Acquired: %s, Netmask: %s, GW: %s",
                     s_ip_info.ip, s_ip_info.netmask, s_ip_info.gateway);

            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            set_state(WIFI_STATE_CONNECTED);

            // Save successful credentials into Flash NVS
            wifi_config_t cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
                wifi_manager_save_credentials((char *)cfg.sta.ssid, (char *)cfg.sta.password);
            }

            // Start SNTP Service if not already running
            if (!esp_sntp_enabled()) {
                ESP_LOGI(TAG, "Starting SNTP time synchronization...");
                esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                sntp_cfg.sync_cb = time_sync_notification_cb;
                esp_netif_sntp_init(&sntp_cfg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Public Functions
// -----------------------------------------------------------------------------
esp_err_t wifi_manager_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi Subsystem...");

    // 1. Initialize NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize TCP/IP Adapter & Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();

    // 3. Initialize Wi-Fi Driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // 5. Set Station Mode and Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_state(WIFI_STATE_IDLE);
    return ESP_OK;
}

void wifi_manager_register_state_cb(wifi_state_cb_t cb, void *user_data) {
    s_state_cb = cb;
    s_state_cb_user_data = user_data;
}

esp_err_t wifi_manager_start_scan(void) {
    if (s_current_state == WIFI_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Cannot scan while connecting.");
        return ESP_ERR_INVALID_STATE;
    }

    set_state(WIFI_STATE_SCANNING);
    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, // scan all channels 1-13
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };

    ESP_LOGI(TAG, "Starting Active Wi-Fi Scan...");
    return esp_wifi_scan_start(&scan_config, false); // false = non-blocking async
}

uint16_t wifi_manager_get_scanned_aps(wifi_ui_ap_t *out_aps, uint16_t max_count) {
    if (!out_aps) return 0;
    uint16_t count = (s_scanned_count < max_count) ? s_scanned_count : max_count;
    memcpy(out_aps, s_scanned_aps, sizeof(wifi_ui_ap_t) * count);
    return count;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    s_user_requested_disconnect = false;
    s_retry_num = 0;
    set_state(WIFI_STATE_CONNECTING);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_LOGI(TAG, "Connecting to SSID: [%s]...", ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect(void) {
    s_user_requested_disconnect = true;
    set_state(WIFI_STATE_DISCONNECTED);
    return esp_wifi_disconnect();
}

wifi_state_t wifi_manager_get_state(void) {
    return s_current_state;
}

int wifi_manager_get_retry_count(void) {
    return s_retry_num;
}

int wifi_manager_get_max_retries(void) {
    return MAX_RETRY_COUNT;
}

bool wifi_manager_get_ip_info(wifi_ip_info_t *out_info) {
    if (!out_info || s_current_state != WIFI_STATE_CONNECTED) return false;
    memcpy(out_info, &s_ip_info, sizeof(wifi_ip_info_t));
    return true;
}

bool wifi_manager_is_time_synced(void) {
    return s_time_synced;
}

void wifi_manager_get_time_str(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%H:%M:%S", &timeinfo);
}

void wifi_manager_get_date_str(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, max_len, "%A, %d %b %Y", &timeinfo);
}

// -----------------------------------------------------------------------------
// NVS Flash Credential Management
// -----------------------------------------------------------------------------
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    const char *pwd = password ? password : "";

    // 1. Read existing stored credentials to avoid redundant Flash NVS writes
    char existing_ssid[WIFI_MAX_SSID_LEN] = {0};
    char existing_pwd[WIFI_MAX_PASSWORD_LEN] = {0};
    if (wifi_manager_load_credentials(existing_ssid, existing_pwd) == ESP_OK) {
        if (strcmp(existing_ssid, ssid) == 0 && strcmp(existing_pwd, pwd) == 0) {
            ESP_LOGI(TAG, "Credentials for [%s] already match NVS. Skipping redundant Flash write.", ssid);
            return ESP_OK;
        }
    }

    // 2. Only write to Flash if credentials have actually changed
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASSWORD, pwd);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "New credentials for [%s] committed to Flash NVS.", ssid);
    return err;
}

esp_err_t wifi_manager_load_credentials(char *ssid, char *password) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t ssid_len = WIFI_MAX_SSID_LEN;
    size_t pwd_len = WIFI_MAX_PASSWORD_LEN;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        nvs_get_str(handle, NVS_KEY_PASSWORD, password, &pwd_len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_clear_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Wi-Fi credentials erased from Flash NVS.");
    return ESP_OK;
}

