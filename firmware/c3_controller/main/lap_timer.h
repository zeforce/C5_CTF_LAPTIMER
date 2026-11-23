/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAP_RECENT_MAX 200u
typedef enum { LAP_ENTER = 1, LAP_EXIT = 2 } lap_edge_t;
typedef struct {
    uint32_t session, sequence;
    uint64_t timestamp_us;
    lap_edge_t edge;
    bool valid;
} lap_event_t;
typedef struct {
    uint32_t minimum_lap_us, total_laps, fastest_lap_number;
    uint64_t fastest_us, start_us, last_timestamp_us;
    uint32_t session, last_sequence;
    uint64_t recent_us[LAP_RECENT_MAX];
    uint16_t recent_count, recent_next;
    bool running, have_session, have_sequence, have_start, armed, exit_seen, interrupted;
} lap_timer_t;

bool lap_timer_init(lap_timer_t *timer, uint32_t minimum_lap_us);
void lap_timer_start(lap_timer_t *timer);
void lap_timer_stop(lap_timer_t *timer);
void lap_timer_invalidate(lap_timer_t *timer);
bool lap_timer_consume(lap_timer_t *timer, const lap_event_t *event, uint64_t *lap_us);
