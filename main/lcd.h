#include "esp_err.h"
#include "tca9534.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "system_init.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

// //generic colour codes for the LCD
// #define COLOUR_RED      0xF800
// #define COLOUR_GREEN    0x07E0
// #define COLOUR_BLUE     0x001F  
// #define COLOUR_YELLOW   0xFFE0
// #define COLOUR_BLACK    0x0000
// #define COLOUR_WHITE    0xFFFF  

// //type of work order
// typedef enum {
//     LCD_FILL_SCREEN,
//     LCD_FILL_RECTANGLE,
// }lcd_cmd_type_t;

// typedef struct{
//     lcd_cmd_type_t cmd_type;
//     uint16_t x1;
//     uint16_t y1;
//     uint16_t x2;
//     uint16_t y2;
//     uint16_t colour;
// }lcd_msg_t;


// //init func
// esp_err_t LCD_Init(void);

// //Function to fill the screen with a specific colour
// esp_err_t LCD_Fill_Screen(uint16_t colour);

// //Function to fill a rectangle with a specific colour
// esp_err_t LCD_Fill_Rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t colour);



#include "lvgl.h"

// Initializes the ST7796S hardware and registers the display with LVGL
esp_err_t lcd_lvgl_init(void);

void spi_ready_cb(spi_transaction_t *trans);

// Power saving: Sleep / Wakeup LCD panel and backlight
void lcd_sleep(void);
void lcd_wakeup(void);

