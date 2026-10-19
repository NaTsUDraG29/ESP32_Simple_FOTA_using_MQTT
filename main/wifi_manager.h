#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of scanned APs to keep in memory
#define MAX_SCANNED_APS         16
#define WIFI_MAX_SSID_LEN       33
#define WIFI_MAX_PASSWORD_LEN   64

// Wi-Fi Connection States
typedef enum {
    WIFI_STATE_UNINITIALIZED = 0,
    WIFI_STATE_IDLE,            // Initialized, not scanning or connecting
    WIFI_STATE_SCANNING,        // Scanning channels for APs
    WIFI_STATE_SCAN_DONE,       // Scan finished, results ready
    WIFI_STATE_CONNECTING,      // Association / 4-way handshake in progress
    WIFI_STATE_CONNECTED,       // Associated and IP received via DHCP
    WIFI_STATE_DISCONNECTED,    // Disconnected from AP
    WIFI_STATE_CONNECT_FAILED   // Password wrong or AP unreachable
} wifi_state_t;

// State Change Notification Callback
typedef void (*wifi_state_cb_t)(wifi_state_t new_state, void *user_data);

// Simplified AP record for UI display
typedef struct {
    char    ssid[WIFI_MAX_SSID_LEN];
    int8_t  rssi;               // Signal strength in dBm (-30 = excellent, -90 = weak)
    bool    is_secured;         // true if WPA/WPA2/WPA3, false if OPEN
    uint8_t authmode;           // wifi_auth_mode_t
} wifi_ui_ap_t;

// IP and Network Info
typedef struct {
    char ip[16];
    char netmask[16];
    char gateway[16];
    char ssid[WIFI_MAX_SSID_LEN];
    int8_t rssi;
} wifi_ip_info_t;

// -----------------------------------------------------------------------------
// Public Functions
// -----------------------------------------------------------------------------

// Initialize NVS, Netif, Default Event Loop, and Wi-Fi Station
esp_err_t wifi_manager_init(void);

// Register a callback for state changes
void wifi_manager_register_state_cb(wifi_state_cb_t cb, void *user_data);

// Trigger an asynchronous scan of all 2.4 GHz Wi-Fi channels
esp_err_t wifi_manager_start_scan(void);

// Retrieve copy of scanned APs (returns count written to out_aps)
uint16_t wifi_manager_get_scanned_aps(wifi_ui_ap_t *out_aps, uint16_t max_count);

// Initiate connection to the given SSID with password
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

// Disconnect from current Wi-Fi network
esp_err_t wifi_manager_disconnect(void);

// Query current Wi-Fi manager state
wifi_state_t wifi_manager_get_state(void);

// Query retry progress
int wifi_manager_get_retry_count(void);
int wifi_manager_get_max_retries(void);

// Query network IP info (valid when state == WIFI_STATE_CONNECTED)
bool wifi_manager_get_ip_info(wifi_ip_info_t *out_info);

// -----------------------------------------------------------------------------
// SNTP & Real-Time Clock
// -----------------------------------------------------------------------------

// Check if system time has been synchronized with NTP
bool wifi_manager_is_time_synced(void);

// Formatted time string (e.g. "14:35:02")
void wifi_manager_get_time_str(char *buf, size_t max_len);

// Formatted date string (e.g. "Wednesday, 14 Oct 2026")
void wifi_manager_get_date_str(char *buf, size_t max_len);

// -----------------------------------------------------------------------------
// NVS Flash Credential Storage
// -----------------------------------------------------------------------------
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
esp_err_t wifi_manager_clear_credentials(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H

