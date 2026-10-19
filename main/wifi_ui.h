#ifndef WIFI_UI_H
#define WIFI_UI_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Wi-Fi UI subsystem and create screens
esp_err_t wifi_ui_init(void);

// Get root screen handle for display loader
lv_obj_t* wifi_ui_get_screen(void);

// Refresh scan list from Wi-Fi manager
void wifi_ui_refresh_ap_list(void);

// Update status label text
void wifi_ui_set_status(const char *text, lv_color_t color);

// Show / Hide the connecting spinner
void wifi_ui_show_spinner(bool show);

// Switch between Wi-Fi Setup Screen and Connected Clock Dashboard
void wifi_ui_show_setup_screen(void);
void wifi_ui_show_clock_screen(void);

// Power saving: notify touch activity to reset screen sleep timer or wake display
void wifi_ui_notify_activity(void);
bool wifi_ui_is_display_sleeping(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_UI_H

