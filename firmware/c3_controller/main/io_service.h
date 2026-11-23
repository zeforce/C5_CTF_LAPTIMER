#pragma once
#include "esp_err.h"
esp_err_t io_service_start(void);
void io_service_diagnostics(void);

#include <stdint.h>
#include <stdbool.h>
esp_err_t io_service_configure(bool light,bool sound,uint8_t brightness);
void io_service_status(uint8_t out[32]);
void io_service_finder_status(uint8_t out[32]);
