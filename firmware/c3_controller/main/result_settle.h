#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "network_sequence.h"
/* Wait at least 2s for final reports, at most 10s for incomplete results. */
static inline bool result_settle_ready(uint64_t elapsed_us, uint16_t missing){
    return elapsed_us>=2000000 && (!missing || elapsed_us>=10000000);
}
static inline bool result_terminal_score(uint32_t terminal_seq,uint32_t pilot_seq){
    return terminal_seq && network_sequence_after(pilot_seq,terminal_seq);
}
