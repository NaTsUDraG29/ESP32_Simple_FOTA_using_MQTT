#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gt911_touch.h"
#include "tca9534.h"
#include "system_init.h"
#include "lvgl.h"
#include "wifi_ui.h"

static const char *TAG = "GT911_LVGL_TOUCH";

// =============================================================================
// 185-Byte GT911 Configuration Array (From Legacy Firmware)
// =============================================================================
// Configures X/Y resolution, filter parameters, touch thresholds, and channel maps.
static uint8_t s_gt911_cfg[185] = {
    0x48, // 0x8047 Config version
    0x40, // 0x8048 X output max low byte (0x01E0 = 480 or 0x0140 = 320)
    0x01, // 0x8049 X output max high byte
    0xE0, // 0x804A Y output max low byte
    0x01, // 0x804B Y output max high byte
    0x0A, // 0x804C Touch number
    0x05, // 0x804D Module switch 1
    0x00, // 0x804E Module switch 2
    0x01, // 0x804F Shake count
    0x08, // 0x8050 Filter
    0x28, // 0x8051 Larger touch
    0x0A, // 0x8052 Noise reduction
    0x50, // 0x8053 Screen touch level
    0x32, // 0x8054 Screen touch leave
    0x03, // 0x8055 Low power control
    0x02, // 0x8056 Refresh rate
    0x00, // 0x8057 X threshold
    0x00, // 0x8058 Y threshold
    0x00, // 0x8059 Reserved
    0x00, // 0x805A Reserved
    0x00, // 0x805B Space top/bottom
    0x00, // 0x805C Space left/right
    0x00, // 0x805D Mini filter
    0x00, // 0x805E Stretch R0
    0x00, // 0x805F Stretch R1
    0x00, // 0x8060 Stretch R2
    0x00, // 0x8061 Stretch RM
    0x87, // 0x8062 Drv groupA num
    0x28, // 0x8063 Drv groupB num
    0x0A, // 0x8064 Sensor num
    0x17, // 0x8065 FreqA factor
    0x15, // 0x8066 FreqB factor
    0x31, // 0x8067 Panel bit freq
    0x0D, // 0x8068 Reserved
    0x00, // 0x8069 Reserved
    0x00, // 0x806A Reserved
    0x00, // 0x806B Panel tx gain
    0xBD, // 0x806C Panel rx gain
    0x04, // 0x806D Panel dump shift
    0x24, // 0x806E Drv frame control
    0x00, // 0x806F Charging level up
    0x00, // 0x8070 Module switch 3
    0x00, // 0x8071 Gesture distance
    0x00, // 0x8072 Gesture long press time
    0x00, // 0x8073 X/Y slope adjust
    0x03, // 0x8074 Gesture control
    0x64, // 0x8075 Gesture switch 1
    0x32, // 0x8076 Gesture switch 2
    0x00, // 0x8077 Gesture refresh rate
    0x00, // 0x8078 Gesture touch level
    0x00, // 0x8079 New green wake up level
    0x40, // 0x807A Freq hopping start
    0x00, // 0x807B Freq hopping end
    0x00, // 0x807C Noise detect time
    0x00, // 0x807D Hopping flag
    0x00, // 0x807E Hopping flag
    0x07, // 0x807F Noise threshold
    0x00, // 0x8080 Noise min threshold
    0x00, // 0x8081 Reserved
    0x00, // 0x8082 Hopping sensor group
    0x00, // 0x8083 Hopping Seg1 normalize
    0x00, // 0x8084 Hopping Seg1 factor
    0x00, // 0x8085 Main clock adjust
    0x00, // 0x8086 Hopping Seg2 normalize
    0x00, // 0x8087 Hopping Seg2 factor
    0x00, // 0x8088 Reserved
    0x00, // 0x8089 Hopping Seg3 normalize
    0x00, // 0x808A Hopping Seg3 factor
    0x00, // 0x808B Reserved
    0x00, // 0x808C Hopping Seg4 normalize
    0x00, // 0x808D Hopping Seg4 factor
    0x00, // 0x808E Reserved
    0x00, // 0x808F Hopping Seg5 normalize
    0x00, // 0x8090 Hopping Seg5 factor
    0x00, // 0x8091 Reserved
    0x00, // 0x8092 Hopping Seg6 normalize
    0x00, // 0x8093 Key1
    0x00, // 0x8094 Key2
    0x00, // 0x8095 Key3
    0x00, // 0x8096 Key4
    0x00, // 0x8097 Key area
    0x00, // 0x8098 Key touch level
    0x00, // 0x8099 Key leave level
    0x00, // 0x809A Key sens
    0x00, // 0x809B Key sens
    0x00, // 0x809C Key restrain
    0x00, // 0x809D Key restrain time
    0x00, // 0x809E Large gesture touch
    0x00, // 0x809F Reserved
    0x00, // 0x80A0 Reserved
    0x00, // 0x80A1 Hotknot noise map
    0x00, // 0x80A2 Link threshold
    0x00, // 0x80A3 Pxy threshold
    0x00, // 0x80A4 GHot dump shift
    0x00, // 0x80A5 GHot rx gain
    0x00, // 0x80A6 Freq gain
    0x00, // 0x80A7 Freq gain 1
    0x00, // 0x80A8 Freq gain 2
    0x00, // 0x80A9 Freq gain 3
    0x00, // 0x80AA Reserved
    0x00, // 0x80AB Reserved
    0x00, // 0x80AC Reserved
    0x00, // 0x80AD Reserved
    0x00, // 0x80AE Reserved
    0x00, // 0x80AF Reserved
    0x00, // 0x80B0 Reserved
    0x00, // 0x80B1 Reserved
    0x00, // 0x80B2 Reserved
    0x00, // 0x80B3 Combine dis
    0x00, // 0x80B4 Split set
    0x00, // 0x80B5 Reserved
    0x00, // 0x80B6 Reserved
    0x02, // 0x80B7 Sensor CH0
    0x04, // 0x80B8 Sensor CH1
    0x06, // 0x80B9 Sensor CH2
    0x08, // 0x80BA Sensor CH3
    0x0A, // 0x80BB Sensor CH4
    0x0C, // 0x80BC Sensor CH5
    0x0E, // 0x80BD Sensor CH6
    0x10, // 0x80BE Sensor CH7
    0x12, // 0x80BF Sensor CH8
    0x14, // 0x80C0 Sensor CH9
    0xFF, // 0x80C1 Sensor CH10
    0xFF, // 0x80C2 Sensor CH11
    0xFF, // 0x80C3 Sensor CH12
    0xFF, // 0x80C4 Sensor CH13
    0xFF, // 0x80C5 Reserved
    0xFF, // 0x80C6 Reserved
    0xFF, // 0x80C7 Reserved
    0xFF, // 0x80C8 Reserved
    0xFF, // 0x80C9 Reserved
    0xFF, // 0x80CA Reserved
    0xFF, // 0x80CB Reserved
    0xFF, // 0x80CC Reserved
    0xFF, // 0x80CD Reserved
    0xFF, // 0x80CE Reserved
    0xFF, // 0x80CF Reserved
    0xFF, // 0x80D0 Reserved
    0xFF, // 0x80D1 Reserved
    0xFF, // 0x80D2 Reserved
    0xFF, // 0x80D3 Reserved
    0xFF, // 0x80D4 Reserved
    0x00, // 0x80D5 Driver CH0
    0x02, // 0x80D6 Driver CH1
    0x04, // 0x80D7 Driver CH2
    0x06, // 0x80D8 Driver CH3
    0x08, // 0x80D9 Driver CH4
    0x0A, // 0x80DA Driver CH5
    0x0C, // 0x80DB Driver CH6
    0x26, // 0x80DC Driver CH7
    0x24, // 0x80DD Driver CH8
    0x22, // 0x80DE Driver CH9
    0x21, // 0x80DF Driver CH10
    0x20, // 0x80E0 Driver CH11
    0x1F, // 0x80E1 Driver CH12
    0x1E, // 0x80E2 Driver CH13
    0x1D, // 0x80E3 Driver CH14
    0xFF, // 0x80E4 Driver CH15
    0xFF, // 0x80E5 Driver CH16
    0xFF, // 0x80E6 Driver CH17
    0xFF, // 0x80E7 Driver CH18
    0xFF, // 0x80E8 Driver CH19
    0xFF, // 0x80E9 Driver CH20
    0xFF, // 0x80EA Driver CH21
    0xFF, // 0x80EB Driver CH22
    0xFF, // 0x80EC Driver CH23
    0xFF, // 0x80ED Driver CH24
    0xFF, // 0x80EE Driver CH25
    0xFF, // 0x80EF Reserved
    0xFF, // 0x80F0 Reserved
    0xFF, // 0x80F1 Reserved
    0xFF, // 0x80F2 Reserved
    0xFF, // 0x80F3 Reserved
    0xFF, // 0x80F4 Reserved
    0xFF, // 0x80F5 Reserved
    0xFF, // 0x80F6 Reserved
    0xFF, // 0x80F7 Reserved
    0xFF, // 0x80F8 Reserved
    0xFF, // 0x80F9 Reserved
    0xFF, // 0x80FA Reserved
    0xFF, // 0x80FB Reserved
    0xFF, // 0x80FC Reserved
    0xFF, // 0x80FD Reserved
    0x74, // 0x80FE Control / Reserved
    0x01  // 0x80FF Checksum byte placeholder (calculated dynamically)
};


// =============================================================================
// Helper Functions: GT911 16-Bit Register Access
// =============================================================================
static esp_err_t gt911_write_regs(uint16_t reg_addr, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);

    // 16-bit register address (Big-Endian: High byte first)
    uint8_t addr_bytes[2] = { (uint8_t)(reg_addr >> 8), (uint8_t)(reg_addr & 0xFF) };
    i2c_master_write(cmd, addr_bytes, 2, true);

    if (data && len > 0) {
        i2c_master_write(cmd, data, len, true);
    }
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t gt911_read_regs(uint16_t reg_addr, uint8_t *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);

    // 16-bit register address (Big-Endian)
    uint8_t addr_bytes[2] = { (uint8_t)(reg_addr >> 8), (uint8_t)(reg_addr & 0xFF) };
    i2c_master_write(cmd, addr_bytes, 2, true);

    // Repeated Start to switch to Read mode
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_I2C_ADDR << 1) | I2C_MASTER_READ, true);

    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Calculate 8-bit Longitudinal Redundancy Check (LRC) checksum
static uint8_t gt911_calc_lrc(const uint8_t *data, uint16_t len) {
    uint8_t lrc = 0;
    for (uint16_t i = 0; i < len; i++) {
        lrc += data[i];
    }
    return (~lrc) + 1; // 2's complement
}


// --- PRIVATE HARDWARE HELPER ---
// Uses your existing I2C code to get raw coordinates
static bool gt911_read_touch(uint16_t *x, uint16_t *y) {
    uint8_t status = 0;
    
    // 1. Read GT911 Status Register 
    esp_err_t ret = gt911_read_regs(GT911_REG_TOUCH_STATUS, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read raw coridnates in GT911! Check address sequence or wiring.");
        return false;
    }

    // If highest bit is 1, buffer is ready. Lower bits are number of touches.
    if ((status & 0x80) && ((status & 0x0F) > 0)) {
        
        uint8_t data[4];
        // 2. Read X/Y Registers (0x8150 to 0x8153)
        ret = gt911_read_regs(GT911_REG_POINT1, data, sizeof(data));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read X and Y in GT911! Check address sequence or wiring.");
            return false;
        }
        
        *x = data[0] | (data[1] << 8);
        *y = data[2] | (data[3] << 8);

        // 3. Clear Status Register to receive the next touch
        uint8_t clear = 0;

        ret = gt911_write_regs(GT911_REG_TOUCH_STATUS, &clear, 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write GT911 status register!");
            return false;
        }       
        return true; // Touch is active
    }
    
    // Even if no touch, we must clear the flag if the buffer was just released
    if (status & 0x80) {
        uint8_t clear = 0;
        ret = gt911_write_regs(GT911_REG_TOUCH_STATUS, &clear, 1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write GT911 status register!");
            return false;
        }      
    }

    return false; // No touch
}


// =============================================================================
// Touch Coordinate Mapping (Landscape 480x320)
// =============================================================================
// The GT911 hardware reports (X: 0..320, Y: 0..480).
// For 480x320 landscape display:
//   - Raw Y (0..480) maps to Screen X (0..479)
//   - Raw X (0..320) maps to Screen Y (0..319)
#define TOUCH_SWAP_XY       1
#define TOUCH_INVERT_X      1
#define TOUCH_INVERT_Y      0

// --- THE LVGL READ CALLBACK ---
static void gt911_lvgl_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static uint16_t last_x = 0;
    static uint16_t last_y = 0;
    uint16_t current_x, current_y;

    bool is_pressed = gt911_read_touch(&current_x, &current_y);

    if (is_pressed) {
        uint16_t mapped_x = current_x;
        uint16_t mapped_y = current_y;

#if TOUCH_SWAP_XY
        mapped_x = current_y;
        mapped_y = current_x;
#endif

#if TOUCH_INVERT_X
        mapped_x = (mapped_x < 480) ? (479 - mapped_x) : 0;
#endif

#if TOUCH_INVERT_Y
        mapped_y = (mapped_y < 320) ? (319 - mapped_y) : 0;
#endif

        if (mapped_x >= 480) mapped_x = 479;
        if (mapped_y >= 320) mapped_y = 319;

        last_x = mapped_x;
        last_y = mapped_y;
        
        bool was_sleeping = wifi_ui_is_display_sleeping();
        wifi_ui_notify_activity(); // Reset sleep timer or wake display

        data->point.x = last_x;
        data->point.y = last_y;
        if (was_sleeping) {
            // First tap was solely to wake the display; suppress click event to avoid accidental button triggers
            data->state = LV_INDEV_STATE_REL;
        } else {
            data->state = LV_INDEV_STATE_PR; // Tell LVGL the screen is PRESSED
        }

        ESP_LOGI(TAG, "Touch Active -> Raw:(%d, %d) | Mapped:(%d, %d)", 
                 current_x, current_y, last_x, last_y);
    } else {
        // LVGL needs the last known coordinates even when releasing
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL; // Tell LVGL the screen is RELEASED
    }
}

// =============================================================================
// GT911 Initialization Sequence
// =============================================================================
esp_err_t gt911_init(void) {
    ESP_LOGI(TAG, "Starting GT911 Hardware Reset and I2C Address Selection...");

    // -------------------------------------------------------------------------
    // STEP 1: Configure Address to 0x5D via Reset & INT Timing Sequence
    // -------------------------------------------------------------------------

    gpio_reset_pin(GT911_INT_GPIO);
    gpio_set_direction(GT911_INT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(GT911_INT_GPIO, 0);

    // Drive CTP_RST (TCA9534 P1) LOW
    tca9534_set_ctp_rst(false);
    vTaskDelay(pdMS_TO_TICKS(10)); // Hold LOW for at least 100 us

    // Set CTP_RST HIGH while holding INT LOW
    tca9534_set_ctp_rst(true);
    vTaskDelay(pdMS_TO_TICKS(60)); // Hold INT LOW for > 5 ms

    // Release INT pin (switch to high-impedance INPUT)
    gpio_set_direction(GT911_INT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GT911_INT_GPIO, GPIO_FLOATING);

    ESP_LOGI(TAG, "Reset complete. GT911 locked to address 0x5D.");

    // -------------------------------------------------------------------------
    // STEP 2: Read Product ID (Sanity Check)
    // -------------------------------------------------------------------------
    uint8_t prod_id[5] = {0};
    esp_err_t ret = gt911_read_regs(GT911_REG_PRODUCT_ID, prod_id, 4);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with GT911 at 0x5D! Check address sequence or wiring.");
        return ret;
    }
    ESP_LOGI(TAG, "Connected to GT911! Product ID: %s", prod_id);

    // -------------------------------------------------------------------------
    // STEP 3: Read Existing Config Version & Apply Custom Config
    // -------------------------------------------------------------------------
    uint8_t current_ver = 0;
    ret = gt911_read_regs(GT911_REG_CONFIG_VERSION, &current_ver, 1);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Current on-chip config version: 0x%02X", current_ver);
        // Use current version (matching legacy logic)
        s_gt911_cfg[0] = current_ver;
    }

    // Calculate checksum of first 184 bytes and store in byte 184 (index 184 -> 0x80FF)
    s_gt911_cfg[sizeof(s_gt911_cfg) - 1] = gt911_calc_lrc(s_gt911_cfg, sizeof(s_gt911_cfg) - 1);
    ESP_LOGI(TAG, "Calculated Config Checksum: 0x%02X", s_gt911_cfg[sizeof(s_gt911_cfg) - 1]);

    // Send 185-byte configuration to 0x8047
    ret = gt911_write_regs(GT911_REG_CONFIG_VERSION, s_gt911_cfg, sizeof(s_gt911_cfg));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write GT911 configuration array!");
        return ret;
    }

    // Notify GT911 that new configuration is ready (Write 1 to 0x8100)
    uint8_t fresh_flag = 1;
    ret = gt911_write_regs(GT911_REG_CONFIG_FRESH, &fresh_flag, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write Config Fresh flag (0x8100)!");
        return ret;
    }

    // Clear initial touch status register
    uint8_t zero = 0;
    gt911_write_regs(GT911_REG_TOUCH_STATUS, &zero, 1);

    ESP_LOGI(TAG, "GT911 initialized successfully!");

    ESP_LOGI(TAG, "Registering Touch to LVGL...");

    // 2. Initialize the input device driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    
    // 3. Configure it as a pointer (touchscreen/mouse) and bind the callback
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = gt911_lvgl_read_cb;
    
    // 4. Register with the LVGL engine
    lv_indev_drv_register(&indev_drv);

    return ESP_OK;
}

// =============================================================================
// Touch Polling & Coordinate Extraction
// // =============================================================================
// bool gt911_read_touch(int16_t *x, int16_t *y) {
//     if (!x || !y) return false;

//     uint8_t status = 0;
//     esp_err_t ret = gt911_read_regs(GT911_REG_TOUCH_STATUS, &status, 1);
//     if (ret != ESP_OK) {
//         return false;
//     }

//     // Bit 7 indicates buffer readiness (1 = touch data available, 0 = no data)
//     if (!(status & 0x80)) {
//         return false;
//     }

//     // Lower 4 bits represent the number of touch points detected (1 to 5)
//     uint8_t num_touches = status & 0x0F;
//     bool touched = false;

//     if (num_touches > 0) {
//         uint8_t pt_data[4] = {0};
//         // Read Point 1 data: X_low, X_high, Y_low, Y_high starting at 0x8150
//         if (gt911_read_regs(GT911_REG_POINT1, pt_data, 4) == ESP_OK) {
//             *x = pt_data[0] | (pt_data[1] << 8);
//             *y = pt_data[2] | (pt_data[3] << 8);
//             touched = true;
//         }
//     }

//     // CRITICAL: Host must write 0x00 to register 0x814E to acknowledge reading
//     // Otherwise, GT911 will never update coordinates again!
//     uint8_t zero = 0;
//     gt911_write_regs(GT911_REG_TOUCH_STATUS, &zero, 1);

//     return touched;
// }

// =============================================================================
// FreeRTOS Background Task
// =============================================================================
// static void gt911_task(void *pvParameters) {
//     ESP_LOGI(TAG, "GT911 Touch Task running. Waiting for touch inputs...");

//     int16_t touch_x = -1;
//     int16_t touch_y = -1;

//     while (1) {
//         if (gt911_read_touch(&touch_x, &touch_y)) {
//             ESP_LOGI(TAG, ">>> Touch Detected: X = %4d, Y = %4d <<<", touch_x, touch_y);
//         }
//         vTaskDelay(pdMS_TO_TICKS(15)); // Poll at ~66 Hz
//     }
// }

// void gt911_start_touch_task(void) {
//     xTaskCreate(gt911_task, "gt911_task", 3072, NULL, 5, NULL);
// }

