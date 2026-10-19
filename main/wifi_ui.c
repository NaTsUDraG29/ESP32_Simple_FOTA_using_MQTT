#include "wifi_ui.h"
#include "wifi_manager.h"
#include "lcd.h"
#include "weather_service.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_UI";

// -----------------------------------------------------------------------------
// UI Handles
// -----------------------------------------------------------------------------
static lv_obj_t *s_scr_setup = NULL;
static lv_obj_t *s_scr_clock = NULL;

// Setup Screen Widgets
static lv_obj_t *s_ap_list = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_btn_rescan = NULL;

// Password Modal Widgets
static lv_obj_t *s_pwd_modal = NULL;
static lv_obj_t *s_pwd_ta = NULL;
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_modal_title = NULL;
static lv_obj_t *s_btn_eye = NULL;
static lv_obj_t *s_eye_label = NULL;
static bool      s_pwd_visible = false;
static char      s_selected_ssid[WIFI_MAX_SSID_LEN] = {0};

// Static storage for AP list item user_data (avoids heap allocation / strdup leaks)
static char      s_list_ssids[MAX_SCANNED_APS][WIFI_MAX_SSID_LEN];

// Clock Dashboard Widgets
static lv_obj_t *s_clock_time_label = NULL;
static lv_obj_t *s_clock_date_label = NULL;
static lv_obj_t *s_clock_sync_label = NULL;
static lv_obj_t *s_clock_ssid_label = NULL;
static lv_obj_t *s_clock_ip_label = NULL;
static lv_obj_t *s_clock_rssi_label = NULL;

// Weather Telemetry Widgets
static lv_obj_t *s_weather_city_label = NULL;
static lv_obj_t *s_weather_temp_label = NULL;
static lv_obj_t *s_weather_cond_label = NULL;
static lv_obj_t *s_weather_sub_label = NULL;

// MQTT Cloud Connection Status Badge
static lv_obj_t *s_clock_mqtt_label = NULL;

// Global Toast Notification Widgets (on lv_layer_top())
static lv_obj_t *s_toast_container = NULL;
static lv_obj_t *s_toast_label = NULL;
static uint32_t  s_toast_hide_time = 0;
static bool      s_toast_visible = false;

// Periodic Timer Handle
static lv_timer_t *s_ui_timer = NULL;

// Inactivity Display Sleep Configuration (30 seconds)
#define DISPLAY_SLEEP_TIMEOUT_MS    30000
static uint32_t s_last_activity_time = 0;
static bool     s_display_is_sleeping = false;

// -----------------------------------------------------------------------------
// Cross-Thread Decoupling Flags (Core 0 Event Callback -> Core 1 GUI Task)
// -----------------------------------------------------------------------------
// LVGL is NOT thread-safe. Wi-Fi & MQTT events trigger on Core 0, but must only be
// rendered on Core 1 by ui_periodic_timer() inside lv_timer_handler().
static volatile bool s_pending_scan_refresh  = false;
static volatile bool s_pending_show_clock    = false;
static volatile bool s_pending_show_setup    = false;
static volatile int  s_pending_spinner       = -1; // -1: none, 0: hide, 1: show
static char          s_pending_status_msg[96] = {0};
static lv_color_t    s_pending_status_color;
static volatile bool s_has_pending_status    = false;

// Toast and Remote Control Decoupling Flags
static char          s_pending_toast_msg[128] = {0};
static uint32_t      s_pending_toast_duration = 0;
static volatile bool s_has_pending_toast     = false;
static volatile bool s_mqtt_status_flag      = false;
static volatile bool s_pending_remote_sleep  = false;
static volatile bool s_pending_remote_wake   = false;

// Forward Declarations
static void create_setup_screen(void);
static void create_clock_screen(void);
static void create_pwd_modal(void);

// -----------------------------------------------------------------------------
// Event Handlers: Password Modal & Keyboard
// -----------------------------------------------------------------------------
static void pwd_modal_connect_cb(void) {
    const char *pwd = lv_textarea_get_text(s_pwd_ta);
    ESP_LOGI(TAG, "Initiating connection to [%s]...", s_selected_ssid);

    if (s_pwd_modal) {
        lv_obj_add_flag(s_pwd_modal, LV_OBJ_FLAG_HIDDEN);
    }

    char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), "Connecting to [%s]...", s_selected_ssid);
    wifi_ui_set_status(status_buf, lv_color_hex(0x38BDF8)); // Sky Blue
    wifi_ui_show_spinner(true);

    wifi_manager_connect(s_selected_ssid, pwd);
}

static void pwd_modal_cancel_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (s_pwd_modal) {
            lv_obj_add_flag(s_pwd_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void eye_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_pwd_visible = !s_pwd_visible;
        if (s_pwd_ta) {
            lv_textarea_set_password_mode(s_pwd_ta, !s_pwd_visible);
        }
        if (s_eye_label) {
            lv_label_set_text(s_eye_label, s_pwd_visible ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
        }
    }
}

static void keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        // User pressed the Enter / Checkmark key on keyboard
        pwd_modal_connect_cb();
    } else if (code == LV_EVENT_CANCEL) {
        if (s_pwd_modal) {
            lv_obj_add_flag(s_pwd_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// -----------------------------------------------------------------------------
// Event Handlers: AP List Selection
// -----------------------------------------------------------------------------
static void ap_item_click_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        const char *ssid = (const char *)lv_event_get_user_data(e);
        if (!ssid) return;

        strncpy(s_selected_ssid, ssid, sizeof(s_selected_ssid) - 1);
        ESP_LOGI(TAG, "User selected AP: [%s]", s_selected_ssid);

        // Update modal title
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "Connect to: %s", s_selected_ssid);
        if (s_modal_title) {
            lv_label_set_text(s_modal_title, title_buf);
        }

        // Reset password text and eye icon to masked mode
        s_pwd_visible = false;
        if (s_pwd_ta) {
            lv_textarea_set_text(s_pwd_ta, "");
            lv_textarea_set_password_mode(s_pwd_ta, true);
        }
        if (s_eye_label) {
            lv_label_set_text(s_eye_label, LV_SYMBOL_EYE_OPEN);
        }

        // Show modal
        if (s_pwd_modal) {
            lv_obj_clear_flag(s_pwd_modal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_pwd_modal);
        }
    }
}

// -----------------------------------------------------------------------------
// Event Handlers: Rescan & Disconnect Buttons
// -----------------------------------------------------------------------------
static void rescan_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Rescan button tapped!");
        wifi_ui_set_status("Scanning for Wi-Fi networks...", lv_color_hex(0xFBBF24)); // Amber
        wifi_ui_show_spinner(true);
        wifi_manager_start_scan();
    }
}

static void disconnect_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Disconnect button tapped!");
        wifi_manager_disconnect();
        s_pending_show_setup = true;
        wifi_manager_start_scan();
    }
}

// -----------------------------------------------------------------------------
// UI Periodic Timer (Runs strictly on Core 1 inside lv_timer_handler)
// -----------------------------------------------------------------------------
static void ui_periodic_timer(lv_timer_t *timer) {
    // 1. Safely process pending status updates
    if (s_has_pending_status) {
        s_has_pending_status = false;
        if (s_status_label) {
            lv_label_set_text(s_status_label, s_pending_status_msg);
            lv_obj_set_style_text_color(s_status_label, s_pending_status_color, LV_PART_MAIN);
        }
    }

    // 2. Safely process pending spinner toggles
    if (s_pending_spinner != -1) {
        if (s_spinner) {
            if (s_pending_spinner == 1) {
                lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_pending_spinner = -1;
    }

    // 3. Safely process pending AP list refresh
    if (s_pending_scan_refresh) {
        s_pending_scan_refresh = false;
        wifi_ui_refresh_ap_list();
    }

    // 4. Safely process pending screen transitions
    if (s_pending_show_clock) {
        s_pending_show_clock = false;
        if (s_scr_clock && lv_scr_act() != s_scr_clock) {
            lv_scr_load(s_scr_clock);
        }
    } else if (s_pending_show_setup) {
        s_pending_show_setup = false;
        if (s_scr_setup && lv_scr_act() != s_scr_setup) {
            lv_scr_load(s_scr_setup);
        }
    }

    // 5. Update Clock and Date Labels when Connected
    wifi_state_t state = wifi_manager_get_state();
    if (state == WIFI_STATE_CONNECTED) {
        char time_buf[32];
        char date_buf[64];
        wifi_manager_get_time_str(time_buf, sizeof(time_buf));
        wifi_manager_get_date_str(date_buf, sizeof(date_buf));

        if (s_clock_time_label) lv_label_set_text(s_clock_time_label, time_buf);
        if (s_clock_date_label) lv_label_set_text(s_clock_date_label, date_buf);

        // Update SNTP Sync Badge
        if (s_clock_sync_label) {
            if (wifi_manager_is_time_synced()) {
                lv_label_set_text(s_clock_sync_label, "✓ SNTP Synced (pool.ntp.org - IST UTC+5:30)");
                lv_obj_set_style_text_color(s_clock_sync_label, lv_color_hex(0x34D399), LV_PART_MAIN); // Green
            } else {
                lv_label_set_text(s_clock_sync_label, "⏳ Synchronizing Time via NTP...");
                lv_obj_set_style_text_color(s_clock_sync_label, lv_color_hex(0xFBBF24), LV_PART_MAIN); // Yellow
            }
        }

        // Update Network Info
        wifi_ip_info_t ip_info;
        if (wifi_manager_get_ip_info(&ip_info)) {
            if (s_clock_ssid_label) lv_label_set_text_fmt(s_clock_ssid_label, "SSID: %s", ip_info.ssid);
            if (s_clock_ip_label)   lv_label_set_text_fmt(s_clock_ip_label,   "IP:   %s", ip_info.ip);
            if (s_clock_rssi_label) lv_label_set_text_fmt(s_clock_rssi_label, "RSSI: %d dBm", ip_info.rssi);
        }

        // Update Live Weather Info
        weather_data_t weather;
        if (weather_service_get_data(&weather)) {
            if (s_weather_city_label) {
                char city_buf[48];
                snprintf(city_buf, sizeof(city_buf), "City: %s", weather.city);
                lv_label_set_text(s_weather_city_label, city_buf);
            }
            if (s_weather_temp_label) {
                char temp_buf[32];
                snprintf(temp_buf, sizeof(temp_buf), "%.1f °C", weather.temperature);
                lv_label_set_text(s_weather_temp_label, temp_buf);
            }
            if (s_weather_cond_label) {
                lv_label_set_text(s_weather_cond_label, weather.condition);
            }
            if (s_weather_sub_label) {
                char sub_buf[64];
                snprintf(sub_buf, sizeof(sub_buf), "Hum: %d%%   Wind: %.1f km/h",
                         weather.humidity, weather.wind_speed);
                lv_label_set_text(s_weather_sub_label, sub_buf);
            }
        }
    }

    // 6. Update MQTT Connection Status Badge in Header
    if (s_clock_mqtt_label) {
        if (s_mqtt_status_flag) {
            lv_label_set_text(s_clock_mqtt_label, "● MQTT: CLOUD CONNECTED");
            lv_obj_set_style_text_color(s_clock_mqtt_label, lv_color_hex(0x10B981), LV_PART_MAIN); // Emerald Green
        } else {
            lv_label_set_text(s_clock_mqtt_label, "○ MQTT: OFFLINE");
            lv_obj_set_style_text_color(s_clock_mqtt_label, lv_color_hex(0x64748B), LV_PART_MAIN); // Slate Gray
        }
    }

    // 7. Manage On-Screen Toast Notifications (Auto-dismissing)
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_has_pending_toast) {
        s_has_pending_toast = false;
        if (s_toast_container && s_toast_label) {
            lv_label_set_text(s_toast_label, s_pending_toast_msg);
            lv_obj_clear_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);
            s_toast_visible = true;
            s_toast_hide_time = now_ms + s_pending_toast_duration;
            // Also wake screen if sleeping so alert is visible!
            if (s_display_is_sleeping) {
                s_display_is_sleeping = false;
                lcd_wakeup();
            }
            s_last_activity_time = now_ms;
        }
    } else if (s_toast_visible) {
        if (now_ms >= s_toast_hide_time) {
            s_toast_visible = false;
            if (s_toast_container) {
                lv_obj_add_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // 8. Safely process remote sleep/wake commands
    if (s_pending_remote_wake) {
        s_pending_remote_wake = false;
        wifi_ui_notify_activity();
    } else if (s_pending_remote_sleep) {
        s_pending_remote_sleep = false;
        if (!s_display_is_sleeping) {
            s_display_is_sleeping = true;
            lcd_sleep();
            ESP_LOGI(TAG, "Display entered sleep via remote MQTT command.");
        }
    }

    // 9. Check Inactivity Display Sleep (30 seconds)
    if (!s_display_is_sleeping && s_last_activity_time > 0 &&
        (now_ms - s_last_activity_time >= DISPLAY_SLEEP_TIMEOUT_MS)) {
        s_display_is_sleeping = true;
        lcd_sleep();
        ESP_LOGI(TAG, "Inactivity timeout (%d s): Display backlight OFF, ST7796S entered Sleep.",
                 DISPLAY_SLEEP_TIMEOUT_MS / 1000);
    }
}

// -----------------------------------------------------------------------------
// Wi-Fi Manager State Change Callback (Invoked on Core 0 by Wi-Fi Task)
// -----------------------------------------------------------------------------
static void on_wifi_state_changed(wifi_state_t new_state, void *user_data) {
    switch (new_state) {
        case WIFI_STATE_SCANNING:
            wifi_ui_notify_activity();
            wifi_ui_set_status("Scanning 2.4 GHz channels...", lv_color_hex(0xFBBF24));
            wifi_ui_show_spinner(true);
            break;

        case WIFI_STATE_SCAN_DONE:
            wifi_ui_notify_activity();
            s_pending_spinner = 0;
            s_pending_scan_refresh = true; // Signal Core 1 GUI task to refresh list safely!
            wifi_ui_set_status("Scan complete. Tap a network to connect.", lv_color_hex(0x94A3B8));
            break;

        case WIFI_STATE_CONNECTING: {
            wifi_ui_notify_activity();
            wifi_ui_show_spinner(true);
            int retries = wifi_manager_get_retry_count();
            if (retries > 0) {
                char retry_msg[64];
                snprintf(retry_msg, sizeof(retry_msg), "Wi-Fi link lost! Reconnecting (%d/%d)...",
                         retries, wifi_manager_get_max_retries());
                wifi_ui_set_status(retry_msg, lv_color_hex(0xFBBF24));
            } else {
                wifi_ui_set_status("Associating & requesting IP via DHCP...", lv_color_hex(0x38BDF8));
            }
            break;
        }

        case WIFI_STATE_CONNECTED:
            wifi_ui_notify_activity();
            s_pending_spinner = 0;
            wifi_ui_set_status("Connected! IP assigned.", lv_color_hex(0x34D399));
            s_pending_show_clock = true; // Signal Core 1 GUI task to switch screen
            break;

        case WIFI_STATE_CONNECT_FAILED:
            wifi_ui_notify_activity();
            s_pending_spinner = 0;
            wifi_ui_set_status("Connection Failed! Router unreachable / retries exhausted.", lv_color_hex(0xEF4444));
            s_pending_show_setup = true; // Transition back to setup screen if on clock screen
            break;

        case WIFI_STATE_DISCONNECTED:
            s_pending_spinner = 0;
            wifi_ui_set_status("Disconnected from Wi-Fi.", lv_color_hex(0xF87171));
            break;

        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Screen Creation: Setup Screen
// -----------------------------------------------------------------------------
static void create_setup_screen(void) {
    s_scr_setup = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_setup, lv_color_hex(0x0A0F1D), LV_PART_MAIN);

    // 1. Header Bar
    lv_obj_t *header = lv_obj_create(s_scr_setup);
    lv_obj_set_size(header, 480, 50);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Wi-Fi Network Setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);

    s_btn_rescan = lv_btn_create(header);
    lv_obj_set_size(s_btn_rescan, 110, 34);
    lv_obj_align(s_btn_rescan, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(s_btn_rescan, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_rescan, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_rescan, rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rescan_label = lv_label_create(s_btn_rescan);
    lv_label_set_text(rescan_label, LV_SYMBOL_REFRESH " RESCAN");
    lv_obj_set_style_text_color(rescan_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(rescan_label);

    // 2. Scanned AP List
    s_ap_list = lv_list_create(s_scr_setup);
    lv_obj_set_size(s_ap_list, 460, 215);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ap_list, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ap_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ap_list, 10, LV_PART_MAIN);

    // 3. Bottom Status Bar
    s_status_label = lv_label_create(s_scr_setup);
    lv_label_set_text(s_status_label, "Initializing Wi-Fi radio...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 15, -12);

    s_spinner = lv_spinner_create(s_scr_setup, 1000, 60);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_align(s_spinner, LV_ALIGN_BOTTOM_RIGHT, -20, -8);
    lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN); // Hidden by default

    // 4. Create the Password Entry Modal (Hidden by default)
    create_pwd_modal();
}

// -----------------------------------------------------------------------------
// Screen Creation: Password Modal with Virtual Keyboard & Eye Toggle
// -----------------------------------------------------------------------------
static void create_pwd_modal(void) {
    s_pwd_modal = lv_obj_create(s_scr_setup);
    lv_obj_set_size(s_pwd_modal, 480, 320);
    lv_obj_align(s_pwd_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_pwd_modal, lv_color_hex(0x0A0F1D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pwd_modal, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pwd_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pwd_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_pwd_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pwd_modal, LV_OBJ_FLAG_HIDDEN); // Hidden until AP tapped

    // Dialog Box Container
    lv_obj_t *dialog = lv_obj_create(s_pwd_modal);
    lv_obj_set_size(dialog, 460, 305);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 12, LV_PART_MAIN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    // Modal Title
    s_modal_title = lv_label_create(dialog);
    lv_label_set_text(s_modal_title, "Enter Password");
    lv_obj_set_style_text_color(s_modal_title, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_modal_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(s_modal_title, LV_ALIGN_TOP_LEFT, 5, 2);

    // Cancel Button (Top Right)
    lv_obj_t *btn_cancel = lv_btn_create(dialog);
    lv_obj_set_size(btn_cancel, 75, 30);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -2, -2);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x475569), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_cancel, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_cancel, pwd_modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    // Password Text Area (W: 360px)
    s_pwd_ta = lv_textarea_create(dialog);
    lv_obj_set_size(s_pwd_ta, 360, 38);
    lv_obj_align(s_pwd_ta, LV_ALIGN_TOP_LEFT, 2, 36);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Password (blank for Open)");
    lv_textarea_set_password_mode(s_pwd_ta, true);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pwd_ta, lv_color_hex(0x475569), LV_PART_MAIN);

    // Eye Button to Unhide/Hide Password (W: 64px)
    s_btn_eye = lv_btn_create(dialog);
    lv_obj_set_size(s_btn_eye, 64, 38);
    lv_obj_align(s_btn_eye, LV_ALIGN_TOP_RIGHT, -2, 36);
    lv_obj_set_style_bg_color(s_btn_eye, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_eye, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_eye, eye_btn_cb, LV_EVENT_CLICKED, NULL);

    s_eye_label = lv_label_create(s_btn_eye);
    lv_label_set_text(s_eye_label, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(s_eye_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(s_eye_label);

    // On-Screen QWERTY Virtual Keyboard
    s_keyboard = lv_keyboard_create(dialog);
    lv_obj_set_size(s_keyboard, 440, 195);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_pwd_ta);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
}

// -----------------------------------------------------------------------------
// Screen Creation: Connected IoT Clock & Dashboard
// -----------------------------------------------------------------------------
static void create_clock_screen(void) {
    s_scr_clock = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_clock, lv_color_hex(0x0A0F1D), LV_PART_MAIN);

    // 1. Top Status Banner
    lv_obj_t *top_banner = lv_obj_create(s_scr_clock);
    lv_obj_set_size(top_banner, 480, 44);
    lv_obj_align(top_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_banner, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(top_banner, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_border_width(top_banner, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(top_banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(top_banner, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_label_create(top_banner);
    lv_label_set_text(badge, "● ONLINE");
    lv_obj_set_style_text_color(badge, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 10, 0);

    // MQTT Cloud Status Badge (Centered in Top Header)
    s_clock_mqtt_label = lv_label_create(top_banner);
    lv_label_set_text(s_clock_mqtt_label, "○ MQTT: OFFLINE");
    lv_obj_set_style_text_color(s_clock_mqtt_label, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_mqtt_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_clock_mqtt_label, LV_ALIGN_CENTER, -15, 0);

    lv_obj_t *btn_disconnect = lv_btn_create(top_banner);
    lv_obj_set_size(btn_disconnect, 130, 30);
    lv_obj_align(btn_disconnect, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(btn_disconnect, lv_color_hex(0xDC2626), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_disconnect, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_disconnect, disconnect_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *disc_lbl = lv_label_create(btn_disconnect);
    lv_label_set_text(disc_lbl, LV_SYMBOL_CLOSE " Disconnect");
    lv_obj_set_style_text_color(disc_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(disc_lbl);

    // 2. Large Digital Clock Card (Center)
    lv_obj_t *clock_card = lv_obj_create(s_scr_clock);
    lv_obj_set_size(clock_card, 460, 160);
    lv_obj_align(clock_card, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(clock_card, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(clock_card, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(clock_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(clock_card, 14, LV_PART_MAIN);
    lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);

    s_clock_time_label = lv_label_create(clock_card);
    lv_label_set_text(s_clock_time_label, "--:--:--");
    lv_obj_set_style_text_color(s_clock_time_label, lv_color_hex(0x38BDF8), LV_PART_MAIN); // Glowing cyan
    lv_obj_set_style_text_font(s_clock_time_label, &lv_font_montserrat_48, LV_PART_MAIN); // Native large 48px font!
    lv_obj_align(s_clock_time_label, LV_ALIGN_TOP_MID, 0, 15);

    s_clock_date_label = lv_label_create(clock_card);
    lv_label_set_text(s_clock_date_label, "Synchronizing date...");
    lv_obj_set_style_text_color(s_clock_date_label, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_date_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(s_clock_date_label, LV_ALIGN_TOP_MID, 0, 78);

    s_clock_sync_label = lv_label_create(clock_card);
    lv_label_set_text(s_clock_sync_label, "⏳ SNTP Sync Pending...");
    lv_obj_set_style_text_color(s_clock_sync_label, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_sync_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_clock_sync_label, LV_ALIGN_TOP_MID, 0, 115);

    // 3. Network Telemetry Card (Bottom Left: 224 x 96)
    lv_obj_t *info_card = lv_obj_create(s_scr_clock);
    lv_obj_set_size(info_card, 224, 96);
    lv_obj_align(info_card, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_set_style_bg_color(info_card, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(info_card, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(info_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(info_card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(info_card, LV_OBJ_FLAG_SCROLLABLE);

    s_clock_ssid_label = lv_label_create(info_card);
    lv_label_set_text(s_clock_ssid_label, "SSID: --");
    lv_obj_set_style_text_color(s_clock_ssid_label, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_ssid_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_clock_ssid_label, LV_ALIGN_TOP_LEFT, 4, 4);

    s_clock_ip_label = lv_label_create(info_card);
    lv_label_set_text(s_clock_ip_label, "IP: 0.0.0.0");
    lv_obj_set_style_text_color(s_clock_ip_label, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_ip_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_clock_ip_label, LV_ALIGN_LEFT_MID, 4, 2);

    s_clock_rssi_label = lv_label_create(info_card);
    lv_label_set_text(s_clock_rssi_label, "RSSI: -- dBm");
    lv_obj_set_style_text_color(s_clock_rssi_label, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_clock_rssi_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_clock_rssi_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    // 4. Live Cloud Weather Card (Bottom Right: 226 x 96)
    lv_obj_t *weather_card = lv_obj_create(s_scr_clock);
    lv_obj_set_size(weather_card, 226, 96);
    lv_obj_align(weather_card, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
    lv_obj_set_style_bg_color(weather_card, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_border_color(weather_card, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_border_width(weather_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(weather_card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(weather_card, LV_OBJ_FLAG_SCROLLABLE);

    s_weather_city_label = lv_label_create(weather_card);
    lv_label_set_text(s_weather_city_label, "City: Bengaluru");
    lv_obj_set_style_text_color(s_weather_city_label, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_weather_city_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_weather_city_label, LV_ALIGN_TOP_LEFT, 4, 4);

    s_weather_temp_label = lv_label_create(weather_card);
    lv_label_set_text(s_weather_temp_label, "--.- °C");
    lv_obj_set_style_text_color(s_weather_temp_label, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_weather_temp_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(s_weather_temp_label, LV_ALIGN_TOP_RIGHT, -4, 2);

    s_weather_cond_label = lv_label_create(weather_card);
    lv_label_set_text(s_weather_cond_label, "Fetching weather...");
    lv_obj_set_style_text_color(s_weather_cond_label, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_weather_cond_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_weather_cond_label, LV_ALIGN_LEFT_MID, 4, 4);

    s_weather_sub_label = lv_label_create(weather_card);
    lv_label_set_text(s_weather_sub_label, "Hum: --%   Wind: -- km/h");
    lv_obj_set_style_text_color(s_weather_sub_label, lv_color_hex(0x64748B), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_weather_sub_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_weather_sub_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

// -----------------------------------------------------------------------------
// Public Functions
// -----------------------------------------------------------------------------
esp_err_t wifi_ui_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi UI Screens...");

    s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
    s_display_is_sleeping = false;

    create_setup_screen();
    create_clock_screen();

    // Create Global Toast Notification Banner on lv_layer_top()
    s_toast_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast_container, 430, 44);
    lv_obj_align(s_toast_container, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(s_toast_container, lv_color_hex(0x1E1B4B), LV_PART_MAIN); // Rich Indigo
    lv_obj_set_style_border_color(s_toast_container, lv_color_hex(0x6366F1), LV_PART_MAIN); // Indigo Accent
    lv_obj_set_style_border_width(s_toast_container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toast_container, 22, LV_PART_MAIN); // Pill shape
    lv_obj_set_style_shadow_width(s_toast_container, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_toast_container, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_toast_container, LV_OPA_70, LV_PART_MAIN);
    lv_obj_clear_flag(s_toast_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);

    s_toast_label = lv_label_create(s_toast_container);
    lv_label_set_text(s_toast_label, "");
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_toast_label);

    // Register callback with Wi-Fi manager
    wifi_manager_register_state_cb(on_wifi_state_changed, NULL);

    // Create 200ms Periodic UI Timer (runs on Core 1 inside lv_timer_handler)
    s_ui_timer = lv_timer_create(ui_periodic_timer, 200, NULL);

    // Boot onto Setup screen
    wifi_ui_show_setup_screen();

    return ESP_OK;
}

lv_obj_t* wifi_ui_get_screen(void) {
    return s_scr_setup;
}

void wifi_ui_refresh_ap_list(void) {
    if (!s_ap_list) return;

    lv_obj_clean(s_ap_list); // Clear old entries

    wifi_ui_ap_t aps[MAX_SCANNED_APS];
    uint16_t count = wifi_manager_get_scanned_aps(aps, MAX_SCANNED_APS);

    if (count == 0) {
        lv_list_add_btn(s_ap_list, LV_SYMBOL_WARNING, "No Wi-Fi Networks Found (Tap Rescan)");
        return;
    }

    for (int i = 0; i < count; i++) {
        // Copy SSID into persistent static slot to avoid dynamic memory allocation
        strncpy(s_list_ssids[i], aps[i].ssid, WIFI_MAX_SSID_LEN - 1);
        s_list_ssids[i][WIFI_MAX_SSID_LEN - 1] = '\0';

        char item_text[64];
        snprintf(item_text, sizeof(item_text), "%-24s  %3d dBm", s_list_ssids[i], aps[i].rssi);

        const char *icon = aps[i].is_secured ? LV_SYMBOL_WIFI : LV_SYMBOL_OK;
        lv_obj_t *btn = lv_list_add_btn(s_ap_list, icon, item_text);

        // Styling
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E293B), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xF8FAFC), LV_PART_MAIN);

        // Pass static string pointer to event callback
        lv_obj_add_event_cb(btn, ap_item_click_cb, LV_EVENT_CLICKED, (void *)s_list_ssids[i]);
    }
}

void wifi_ui_set_status(const char *text, lv_color_t color) {
    if (text) {
        strncpy(s_pending_status_msg, text, sizeof(s_pending_status_msg) - 1);
        s_pending_status_color = color;
        s_has_pending_status = true;
    }
}

void wifi_ui_show_spinner(bool show) {
    s_pending_spinner = show ? 1 : 0;
}

void wifi_ui_show_setup_screen(void) {
    s_pending_show_setup = true;
}

void wifi_ui_show_clock_screen(void) {
    s_pending_show_clock = true;
}

void wifi_ui_notify_activity(void) {
    s_last_activity_time = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_display_is_sleeping) {
        s_display_is_sleeping = false;
        lcd_wakeup();
        ESP_LOGI(TAG, "Display woke up on user touch.");
    }
}

bool wifi_ui_is_display_sleeping(void) {
    return s_display_is_sleeping;
}

void wifi_ui_set_mqtt_status(bool connected) {
    s_mqtt_status_flag = connected;
}

void wifi_ui_show_toast(const char *msg, uint32_t duration_ms) {
    if (msg) {
        snprintf(s_pending_toast_msg, sizeof(s_pending_toast_msg), "%s  %s", LV_SYMBOL_BELL, msg);
        s_pending_toast_duration = (duration_ms > 0) ? duration_ms : 4000;
        s_has_pending_toast = true;
    }
}

void wifi_ui_sleep_display(void) {
    s_pending_remote_sleep = true;
}

void wifi_ui_wake_display(void) {
    s_pending_remote_wake = true;
}

