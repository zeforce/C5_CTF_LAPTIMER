#pragma once
#include <stdint.h>
#include <stdbool.h>
/* RGB logical color -> WS2812 GRB, brightness 0..255, nearest rounding. */
static inline void ws2812_grb(uint32_t rgb, uint8_t brightness, bool lit, uint8_t out[3]) {
    const unsigned shifts[3]={8,16,0};
    for(unsigned i=0;i<3;i++)out[i]=lit?((((rgb>>shifts[i])&255)*brightness+127)/255):0;
}
