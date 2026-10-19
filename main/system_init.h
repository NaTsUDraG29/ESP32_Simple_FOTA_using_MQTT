#ifndef SYSTEM_INIT_H
#define SYSTEM_INIT_H

/*
 * Shared board setup definitions for the ESP32 RTOS bring-up project.
 */

#include "driver/spi_master.h"

#define I2C_SCL_PIN             27
#define I2C_SDA_PIN             26
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_SLAVE_ADDR          0x27

#define TCA9534_OUTPUT_PORT     0x01
#define TCA9534_CONFIG          0x03

#define LCD_MOSI_PIN            4
#define LCD_CLK_PIN             5
#define LCD_DC_PIN              2
#define LCD_HOST                SPI2_HOST
#define SPI_CLK_SPEED_HZ        25000000

#define TCA9534_I2C_ADDR            0x27
#define TCA9534_OUTPUT_PORT_REG     0x01
#define TCA9534_CONFIG_REG          0x03
#define TCA9534_PIN_CTP_RST         (1 << 1)

#define GT911_INT_GPIO              GPIO_NUM_15
#define GT911_I2C_ADDR              0x5D

#define GT911_REG_CONFIG_VERSION    0x8047
#define GT911_REG_CONFIG_CHECKSUM   0x80FF
#define GT911_REG_CONFIG_FRESH      0x8100
#define GT911_REG_PRODUCT_ID        0x8140
#define GT911_REG_TOUCH_STATUS      0x814E
#define GT911_REG_POINT1            0x8150

extern spi_device_handle_t spi;

esp_err_t System_Init(void);

#endif // SYSTEM_INIT_H