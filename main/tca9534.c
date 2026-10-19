#include "tca9534.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "system_init.h"


static const char* TAG = "TCA9534";
// =============================================================================
// Helper Functions: TCA9534 I/O Expander
// =============================================================================
esp_err_t tca9534_set_ctp_rst(bool level) {
    uint8_t out_val = 0;
    // Read current output register
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, 
                                                 (uint8_t[]){TCA9534_OUTPUT_PORT_REG}, 1, 
                                                 &out_val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read TCA9534 output register: %s", esp_err_to_name(ret));
        return ret;
    }

    if (level) {
        out_val |= TCA9534_PIN_CTP_RST;  // High
    } else {
        out_val &= ~TCA9534_PIN_CTP_RST; // Low
    }

    uint8_t write_buf[2] = { TCA9534_OUTPUT_PORT_REG, out_val };
    return i2c_master_write_to_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(100));
}


esp_err_t tca9534_read_output_reg(uint8_t reg_addr, uint8_t *out_val) {
    if (!out_val) return ESP_ERR_INVALID_ARG;
    return i2c_master_write_read_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, 
                                        (uint8_t[]){reg_addr}, 1, 
                                        out_val, 1, pdMS_TO_TICKS(100));
}

esp_err_t tca9534_set_output_reg(uint8_t reg_addr, uint8_t out_val) {
    uint8_t write_buf[2] = { reg_addr, out_val };
    return i2c_master_write_to_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(100));
}

esp_err_t TCA9534_Init(void) {
    // Configure all pins as outputs. Bit 7 is left low so the GPIO expander
    // does not leave any pin in input mode for the LCD / touch routing.
    uint8_t config_val = 0x80;
    uint8_t write_buf[2] = { TCA9534_CONFIG_REG, config_val };
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9534: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set all outputs LOW initially
    uint8_t out_val = 0x00;
    write_buf[0] = TCA9534_OUTPUT_PORT_REG;
    write_buf[1] = out_val;
    ret = i2c_master_write_to_device(I2C_MASTER_NUM, TCA9534_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set TCA9534 outputs LOW: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TCA9534 initialized successfully.");

    //create a task to read/write the pinouts -maybe lets see

    return ESP_OK;
}