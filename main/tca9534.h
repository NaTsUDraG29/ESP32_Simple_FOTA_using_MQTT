/*
Keshava D Hegde
@brief To initialize the TCA 9534 IO expander
*/


#include "esp_err.h"

esp_err_t TCA9534_Init(void);

esp_err_t tca9534_set_ctp_rst(bool level);

esp_err_t tca9534_set_output_reg(uint8_t reg_addr, uint8_t out_val);

esp_err_t tca9534_read_output_reg(uint8_t reg_addr, uint8_t *out_val);