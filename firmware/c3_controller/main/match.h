#pragma once
#include <stdint.h>
#include <stdbool.h>
#define MATCH_NODES 9
typedef struct {
    uint32_t session, age_ms;
    uint8_t mode, state, fault;
    uint32_t team_ms[2], pilot_ms[8], pilot_laps[8], pilot_fastest_ms[8],pilot_captures[8];
} match_node_t;
typedef struct {
    uint32_t session;
    uint16_t expected_mask, included_mask, missing_mask;
    uint8_t mode, fault_count;
    uint64_t team_ms[2], pilot_ms[8];
    uint32_t pilot_laps[8],pilot_fastest_ms[8],pilot_captures[8];
} match_summary_t;
/* Only same-session, same-mode, fresh samples enter a total. */
void match_summarize(uint32_t session, uint8_t mode, uint16_t expected,
                     const match_node_t nodes[MATCH_NODES], match_summary_t *out);
