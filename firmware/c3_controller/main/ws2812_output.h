#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t ws2812_output_init(int gpio, unsigned count, uint8_t brightness);
/* Single task caller; unchanged frames are skipped, busy frames retried next tick. */
esp_err_t ws2812_output_update(uint32_t rgb, bool lit);

/* Called only from the same worker as update. */
void ws2812_output_brightness(uint8_t brightness);
