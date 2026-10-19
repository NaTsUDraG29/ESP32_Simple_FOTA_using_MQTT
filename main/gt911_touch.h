#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the GT911 touch controller.
 *        Performs the hardware reset via TCA9534 (P1) and ESP32 GPIO 15 (INT),
 *        locks the I2C address to 0x5D, reads product ID, and uploads configuration.
 * 
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t gt911_init(void);

// /**
//  * @brief Poll the GT911 for touch input.
//  * 
//  * @param[out] x Pointer to store the X coordinate (0-479 or 0-319 depending on orientation)
//  * @param[out] y Pointer to store the Y coordinate
//  * @return true if screen is currently touched, false otherwise.
//  */
// static bool gt911_read_touch(int16_t *x, int16_t *y);

// /**
//  * @brief Start a background FreeRTOS task that polls the GT911 and logs coordinates to the console.
//  */
// void gt911_start_touch_task(void);

#ifdef __cplusplus
}
#endif

