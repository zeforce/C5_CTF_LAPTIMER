#pragma once
#include <stdint.h>
typedef struct { uint32_t age_ms,session,boot;uint8_t state,mode; } network_guard_node_t;
/* A brief mismatch is allowed while the sequential start/resume ACKs arrive. */
uint16_t network_guard_missing(uint32_t session,uint8_t mode,uint16_t roster,uint32_t since_start_ms,
    const uint32_t expected_boot[8],const network_guard_node_t nodes[8]);
