#include "lcd.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "system_init.h"

// static const char* TAG = "LCD_INIT";

// static QueueHandle_t lcd_command_handle = NULL;

// static void lcd_cs(bool active) {
//     if (active) {
//         // CS LOW (Selected). Bit 6=1, 5=1, 4=0, 2=1
//         TCA9534_write_register(TCA9534_OUTPUT_PORT, 0b01100110); 
//     } else {
//         // CS HIGH (Deselected). Bit 6=1, 5=1, 4=1, 2=1
//         TCA9534_write_register(TCA9534_OUTPUT_PORT, 0b01110110); 
//     }
// }

// static void lcd_draw_rect_dma_internal(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color) {
//     uint16_t width = x2 - x1 + 1;
//     uint16_t height = y2 - y1 + 1;
//     size_t row_bytes = width * 2; // 16-bit RGB565 = 2 bytes per pixel
    
//     // 2. Allocate DMA memory for exactly ONE row (Safe for ESP32 RAM limits)
//     uint8_t *color_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
//     if (color_buf == NULL) {
//         ESP_LOGE(TAG, "Failed to allocate DMA memory for LCD row!");
//         return;
//     }

//     // 3. Fill the row buffer with the 16-bit color
//     uint8_t color_high = color >> 8;
//     uint8_t color_low = color & 0xFF;
//     for (int i = 0; i < width; i++) {
//         color_buf[i * 2] = color_high;
//         color_buf[i * 2 + 1] = color_low;
//     }

//     // 4. Lock the SPI bus
//     lcd_cs(true);

//     // 5. Define the drawing window boundaries in the ST7796S
//     lcd_send_cmd(0x2A); // Column Address Set
//     lcd_send_data(x1 >> 8); lcd_send_data(x1 & 0xFF);    
//     lcd_send_data(x2 >> 8); lcd_send_data(x2 & 0xFF); 

//     lcd_send_cmd(0x2B); // Row Address Set
//     lcd_send_data(y1 >> 8); lcd_send_data(y1 & 0xFF);    
//     lcd_send_data(y2 >> 8); lcd_send_data(y2 & 0xFF); 

//     lcd_send_cmd(0x2C); // Memory Write (RAMWR)

//     // 6. Blast the pixels row by row using high-speed DMA
//     gpio_set_level(LCD_DC_PIN, 1); // Set Data Mode
//     esp_rom_delay_us(1);           // Let voltage settle

//     for (int y = 0; y < height; y++) {
//         spi_transaction_t t = {
//             .length = row_bytes * 8,  // SPI length is always in BITS
//             .tx_buffer = color_buf
//         };
//         // Because we initialized the bus with SPI_DMA_CH_AUTO, this automatically uses DMA
//         spi_device_polling_transmit(spi, &t);
//     }
    
//     // 7. Release the bus and free the memory
//     lcd_cs(false);
//     free(color_buf);
// }



// static void lcd_task(void* pvParameters)
// {
//     lcd_msg_t lcd_msg;

//     while(true)
//     {
//         if(xQueueReceive(&lcd_command_handle, &lcd_msg, portMAX_DELAY) == pdTRUE)
        
//         if(lcd_msg.cmd_type == LCD_FILL_SCREEN)
//         {
//             //SPI command to fill screen using DMA
//             lcd_draw_rect_dma_internal(0, 0, 479, 319, lcd_msg.colour);
            
//         }
//         else if(lcd_msg.cmd_type == LCD_FILL_SCREEN)
//         {
//             //SPI command to fill screen using DMA
//             lcd_draw_rect_dma_internal(lcd_msg.x1, lcd_msg.y1, lcd_msg.x2, lcd_msg.y2, lcd_msg.colour);

//         }

//     }
// }

// //Init fucntion

// esp_err_t LCD_Init(void) {

//     gpio_reset_pin(LCD_DC_PIN);
//     gpio_set_direction(LCD_DC_PIN, GPIO_MODE_OUTPUT);

//     //set the LCD_RST -low, LCD_BT - HIGH, LCD_PWR-HIGH and LCD_CS - LOW
//     if(tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01100110) != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to set initial output register for LCD");
//         return ESP_FAIL;
//     }

//     vTaskDelay(pdMS_TO_TICKS(50)); // Wait for 50ms

//     if(tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01100110) != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to set initial output register for LCD");
//         return ESP_FAIL;
//     }

//     vTaskDelay(pdMS_TO_TICKS(100)); // Wait for 100ms

//     //create the Queue
//     lcd_command_handle = xQueueCreate(10,sizeof(lcd_msg_t));

//     //create a task

//     xTaskCreate(lcd_task,
//                 "lcd_task",
//                 2048,
//                 NULL,
//                 4,
//                 NULL);

//     ESP_LOGI(TAG, "LCD Initialized Successfully!");
//     return ESP_OK;
// }


// //function to fill the screen with a specific colour
// esp_err_t LCD_Fill_Screen(uint16_t colour)
// {
//     if(lcd_command_handle == NULL) return ESP_ERR_NO_MEM;

//     lcd_msg_t lcd_msg = {
//         .cmd_type = LCD_FILL_SCREEN,
//         .colour = colour,
//     };

//     xQueueSend(lcd_command_handle, &lcd_msg, portMAX_DELAY);
// }


// //function to fill a rectangle with a specific colour
// esp_err_t LCD_Fill_Rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t colour)
// {

//     if(lcd_command_handle == NULL) return ESP_ERR_NO_MEM;

//     lcd_msg_t lcd_msg = {

//         .cmd_type = LCD_FILL_RECTANGLE,
//         .x1 = x1,
//         .y1 = y1,
//         .x2 = x2,
//         .y2 = y2,
//         .colour = colour,
//     };

//     xQueueSend(lcd_command_handle,&lcd_msg,portMAX_DELAY);
// }


static const char *TAG = "LCD_LVGL";

// --- PRIVATE HARDWARE HELPERS ---
static void lcd_send_cmd(uint8_t command) {
    gpio_set_level(LCD_DC_PIN, 0); 
    esp_rom_delay_us(1);          
    spi_transaction_t t = {0}; 
    t.length = 8; 
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = command;
    spi_device_polling_transmit(spi, &t);
}

static void lcd_send_data(uint8_t data) {
    gpio_set_level(LCD_DC_PIN, 1); 
    esp_rom_delay_us(1);
    spi_transaction_t t = {0};           
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi, &t);
}

// --- THE LVGL FLUSH CALLBACK ---
// LVGL calls this when it has a finished chunk of pixels ready to draw.
// We send the window coordinates, transmit the pixel buffer via SPI DMA synchronously,
// and immediately notify LVGL that flushing is complete to avoid Watchdog timeouts.
static void lcd_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    uint32_t num_pixels = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    size_t bytes_to_send = num_pixels * sizeof(lv_color_t);
    
    // Set column boundaries (0x2A)
    lcd_send_cmd(0x2A); 
    lcd_send_data(area->x1 >> 8); lcd_send_data(area->x1 & 0xFF);    
    lcd_send_data(area->x2 >> 8); lcd_send_data(area->x2 & 0xFF); 

    // Set page/row boundaries (0x2B)
    lcd_send_cmd(0x2B); 
    lcd_send_data(area->y1 >> 8); lcd_send_data(area->y1 & 0xFF);    
    lcd_send_data(area->y2 >> 8); lcd_send_data(area->y2 & 0xFF); 

    // Write to RAM (0x2C)
    lcd_send_cmd(0x2C); 
    gpio_set_level(LCD_DC_PIN, 1);
    esp_rom_delay_us(1);
    
    // Synchronous DMA transmission: transmits the entire buffer efficiently
    spi_transaction_t trans = {0};
    trans.length = bytes_to_send * 8; // Length in bits
    trans.tx_buffer = color_map;

    spi_device_polling_transmit(spi, &trans);

    // CRITICAL: Notify LVGL immediately that the buffer is free!
    // This clears draw_buf->flushing and prevents the Task Watchdog from triggering.
    lv_disp_flush_ready(drv); 
}

// --- INITIALIZATION ---
esp_err_t lcd_lvgl_init(void) {
    
    ESP_LOGI(TAG, "Initializing LVGL hardware...");
    
    lv_init();

    ESP_LOGI(TAG, "Initializing ST7796S hardware...");
    
    gpio_reset_pin(LCD_DC_PIN);
    gpio_set_direction(LCD_DC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_DC_PIN, 1);

    // Power up LCD and perform hardware reset sequence via TCA9534:
    // P6 (LCD_PWR) = 1, P5 (LCD_BL) = 1, P4 (LCD_CS) = 0, P2 (LCD_RST) = 0
    tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01100010); 
    vTaskDelay(pdMS_TO_TICKS(50)); // Wait for 3V3_LCD power rail to stabilize

    // Release Reset: P2 (LCD_RST) = 1
    tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01100110); 
    vTaskDelay(pdMS_TO_TICKS(120)); // Wait for internal controller reset

    // --- ST7796S Controller Initialization Sequence ---
    ESP_LOGI(TAG, "Sending ST7796S initialization commands...");
    lcd_send_cmd(0x01); // Software Reset
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_send_cmd(0x11); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));

    // ST7796S Command Set 2 Unlock
    lcd_send_cmd(0xF0);
    lcd_send_data(0xC3);
    lcd_send_cmd(0xF0);
    lcd_send_data(0x96);

    // Memory Data Access Control (MADCTL):
    // 0xE8 sets MV=1 (Landscape 480x320) + MY=1, MX=1 (180-degree flipped orientation) + BGR panel order.
    lcd_send_cmd(0x36);
    lcd_send_data(0xE8);

    // Interface Pixel Format: 16-bit RGB565
    lcd_send_cmd(0x3A);
    lcd_send_data(0x55);

    // Display Inversion Control
    lcd_send_cmd(0xB4);
    lcd_send_data(0x01);

    // Normal Display Mode On
    lcd_send_cmd(0x13);

    // Display On
    lcd_send_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "Registering Display to LVGL...");

    // Allocate LVGL rendering buffer (40 rows is the sweet spot for ESP32 RAM)
    static lv_disp_draw_buf_t draw_buf;
    lv_color_t *buf1 = heap_caps_malloc(480 * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to allocate DMA buffer!");
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 480 * 40);

    // Initialize and register the LVGL display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    
    disp_drv.hor_res = 480;
    disp_drv.ver_res = 320;
    disp_drv.flush_cb = lcd_lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    
    lv_disp_drv_register(&disp_drv);
    ESP_LOGI(TAG, "Display registered to LVGL successfully.");

    return ESP_OK;
}

void lcd_sleep(void) {
    lcd_send_cmd(0x10); // ST7796S Sleep In
    // Turn OFF backlight LED (clear Bit 5 of TCA9534): 0b01000110
    tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01000110);
    ESP_LOGI(TAG, "LCD Backlight OFF, ST7796S Asleep.");
}

void lcd_wakeup(void) {
    // Turn ON backlight LED (set Bit 5 of TCA9534): 0b01100110
    tca9534_set_output_reg(TCA9534_OUTPUT_PORT_REG, 0b01100110);
    lcd_send_cmd(0x11); // ST7796S Sleep Out
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_send_cmd(0x29); // Display ON
    ESP_LOGI(TAG, "LCD Backlight ON, ST7796S Awake.");
}