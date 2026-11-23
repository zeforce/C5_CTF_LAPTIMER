/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

/* Additive contract revision. Existing command payloads remain unchanged. */
#define NODE_CONTRACT_REVISION 5u
#define NODE_CMD_CAPABILITIES 10u
#define NODE_CMD_CLOCK 11u
#define NODE_CMD_OBSERVATION_DETAIL 12u
#define NODE_CMD_GET_LIMITS 13u
#define NODE_CMD_SET_LIMITS 14u
#define NODE_CMD_SCAN_START 15u
#define NODE_CMD_SCAN_STATUS 16u
#define NODE_CMD_SCAN_CANCEL 17u
#define NODE_CMD_SCAN_RESULT 18u
#define NODE_CMD_HEALTH 19u
#define NODE_CMD_RECOVER 20u
#define NODE_CMD_CHANNEL_INFO 21u
#define NODE_FAULT_RF_INIT (1u << 0)
#define NODE_FAULT_RF_STALE (1u << 1)
#define NODE_FAULT_EVENT_OVERFLOW (1u << 2)
#define NODE_FAULT_SPI_INIT (1u << 3)
#define NODE_FAULT_RX_ERRORS (1u << 4)
#define NODE_QUALITY_SETTLING (1u << 5)
#define NODE_CAP_TIMESTAMPS (1u << 0)
#define NODE_CAP_QUALITY (1u << 1)
#define NODE_CAP_EVENT_INT (1u << 2)
#define NODE_CAP_ASYNC_SCAN (1u << 3)
#define NODE_CAP_LIMITS (1u << 4)
#define NODE_CAP_RETRY (1u << 5)
#define NODE_CAP_HEALTH (1u << 6)
#define NODE_CAP_CHANNEL_INFO (1u << 7)
#define NODE_CAPABILITIES (NODE_CAP_TIMESTAMPS | NODE_CAP_QUALITY | NODE_CAP_EVENT_INT | NODE_CAP_ASYNC_SCAN | NODE_CAP_LIMITS | NODE_CAP_RETRY | NODE_CAP_HEALTH | NODE_CAP_CHANNEL_INFO)
#define NODE_QUALITY_NO_SAMPLE (1u << 0)
#define NODE_QUALITY_STALE (1u << 1)
#define NODE_QUALITY_CLIPPED (1u << 2)
#define NODE_QUALITY_LOW_VARIATION (1u << 3)
#define NODE_QUALITY_OVERFLOW (1u << 4)
#define NODE_OBSERVATION_STALE_US 100000u
#define NODE_CLIP_LIMIT_PM 100u
#define NODE_CHANNEL_COUNT 64u
#define NODE_EVENT_CAPACITY 16u

static inline uint32_t node_stale_us(uint16_t observation_ms)
{
    uint32_t period = (uint32_t)observation_ms * 2000u;
    return period > NODE_OBSERVATION_STALE_US ? period : NODE_OBSERVATION_STALE_US;
}

static inline uint8_t node_quality_flags(uint32_t samples, uint32_t varied,
                                        uint16_t clip, uint32_t overflows,
                                        uint64_t timestamp, uint64_t now)
{
    uint8_t flags = 0;
    if (!samples || !timestamp) flags |= NODE_QUALITY_NO_SAMPLE;
    if (!timestamp || now < timestamp || now - timestamp > NODE_OBSERVATION_STALE_US)
        flags |= NODE_QUALITY_STALE;
    if (clip >= NODE_CLIP_LIMIT_PM) flags |= NODE_QUALITY_CLIPPED;
    if (samples && (uint64_t)varied * 2u < samples) flags |= NODE_QUALITY_LOW_VARIATION;
    if (overflows) flags |= NODE_QUALITY_OVERFLOW;
    return flags;
}

static inline int node_empty_request(uint8_t command)
{
    return command <= 3 || command == 5 || command == 6 ||
        command == NODE_CMD_CAPABILITIES || command == NODE_CMD_CLOCK ||
        command == NODE_CMD_OBSERVATION_DETAIL;
}
