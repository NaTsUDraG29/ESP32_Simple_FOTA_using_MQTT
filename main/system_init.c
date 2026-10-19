
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "system_init.h"
#include "lvgl.h"
#include "lcd.h"
#include "esp_err.h"



static const char *TAG = "SYSTEM_INIT";

spi_device_handle_t spi;

// void IRAM_ATTR spi_ready_cb(spi_transaction_t *trans);

// --- I2C & CS Helpers ---
static void I2C_Init() {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);  
}

// --- SPI Setup ---
void LCD_SPI_init() {
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = LCD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 65536, // Allocate enough transfer bandwidth for 480*40*2 (38.4 KB)
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLK_SPEED_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
        .post_cb = NULL,
    };

    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &spi));
}

esp_err_t System_Init(void)
{
    //initilaise I2C
    I2C_Init();
    ESP_LOGI(TAG, "I2C initialised");
    //inisitalise SPI

    LCD_SPI_init();
    ESP_LOGI(TAG, "SPI initialised");

    return ESP_OK;
}



