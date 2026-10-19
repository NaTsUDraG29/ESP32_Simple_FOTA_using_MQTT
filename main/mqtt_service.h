#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// MQTT Service Configuration Defaults
// -----------------------------------------------------------------------------
#define MQTT_DEFAULT_BROKER_URI         "mqtt://broker.emqx.io:1883"
#define MQTT_TELEMETRY_INTERVAL_SEC     15
#define MQTT_MAX_TOPIC_LEN              96
#define MQTT_MAX_PAYLOAD_LEN            1024

// MQTT Status Callback
typedef void (*mqtt_status_cb_t)(bool connected, void *user_data);

/**
 * @brief Initialize MQTT service, derive unique Client ID & topics,
 *        and hook into Wi-Fi connection events (runs on Core 0).
 */
esp_err_t mqtt_service_init(void);

/**
 * @brief Register a callback for MQTT connection/disconnection events.
 */
void mqtt_service_register_status_cb(mqtt_status_cb_t cb, void *user_data);

/**
 * @brief Query if MQTT client is currently connected to the broker.
 */
bool mqtt_service_is_connected(void);

/**
 * @brief Publish a message to a specific MQTT topic.
 */
esp_err_t mqtt_service_publish(const char *topic, const char *data, int qos, int retain);

/**
 * @brief Trigger an immediate telemetry publication (bypasses periodic interval).
 */
void mqtt_service_trigger_telemetry(void);

/**
 * @brief Retrieve topic strings and client identifier for display or logging.
 */
const char* mqtt_service_get_client_id(void);
const char* mqtt_service_get_telemetry_topic(void);
const char* mqtt_service_get_cmd_topic(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_SERVICE_H

