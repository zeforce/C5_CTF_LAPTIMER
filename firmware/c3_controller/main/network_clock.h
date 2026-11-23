#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

/* Conservative 1000ppm drift allowance; expires after 10 seconds.
 * This is a configured bound, not a measured oscillator specification. */
static inline uint32_t network_clock_error(int64_t sampled,int64_t now,uint32_t error){
    if(sampled<=0 || now<sampled || now-sampled>=10000000)return UINT32_MAX;
    uint64_t bound=(uint64_t)error+((uint64_t)(now-sampled)+999)/1000;
    return bound>UINT32_MAX?UINT32_MAX:(uint32_t)bound;
}
/* A delayed radio exchange must not replace a more precise recent sample.
 * Age the old error before comparing, so samples cannot live indefinitely. */
static inline bool network_clock_select(int64_t sampled,int64_t now,uint32_t previous,
                                        uint32_t candidate){
    return candidate<=network_clock_error(sampled,now,previous);
}
