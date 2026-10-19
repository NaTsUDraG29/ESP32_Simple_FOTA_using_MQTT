#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Weather Data Structure
typedef struct {
    float   temperature;        // e.g. 26.5 °C
    int     humidity;           // e.g. 65 %
    float   wind_speed;         // e.g. 12.4 km/h
    int     weather_code;       // WMO weather code (0..99)
    char    condition[32];      // e.g. "Sunny", "Partly Cloudy", "Rain Showers"
    char    city[32];           // e.g. "Bengaluru"
    bool    valid;              // true when valid data has been parsed
    uint32_t last_update_sec;   // Unix timestamp of last update
} weather_data_t;

// Weather State Change Callback
typedef void (*weather_update_cb_t)(const weather_data_t *data, void *user_data);

// Initialize weather service background task (pinned to Core 0)
esp_err_t weather_service_init(void);

// Register a callback to be notified when new weather data is available
void weather_service_register_cb(weather_update_cb_t cb, void *user_data);

// Trigger an immediate weather update (bypasses the periodic timer)
void weather_service_trigger_refresh(void);

// Thread-safe copy of latest weather data (returns true if data is valid)
bool weather_service_get_data(weather_data_t *out_data);

// Configure location coordinates (defaults to Bengaluru: 12.9716 N, 77.5946 E)
void weather_service_set_location(float lat, float lon, const char *city_name);

#ifdef __cplusplus
}
#endif

#endif // WEATHER_SERVICE_H

