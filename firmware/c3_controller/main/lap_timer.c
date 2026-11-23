/* SPDX-License-Identifier: GPL-3.0-only */
#include "lap_timer.h"
#include <string.h>

bool lap_timer_init(lap_timer_t *timer, uint32_t minimum_lap_us)
{
    if (!timer || !minimum_lap_us) return false;
    memset(timer, 0, sizeof(*timer));
    timer->minimum_lap_us = minimum_lap_us;
    timer->armed = true;
    return true;
}

void lap_timer_start(lap_timer_t *timer)
{
    uint32_t minimum = timer->minimum_lap_us;
    (void)lap_timer_init(timer, minimum);
    timer->running = true;
}

void lap_timer_stop(lap_timer_t *timer)
{
    timer->running = false;
    timer->have_start = false;
}

void lap_timer_invalidate(lap_timer_t *timer)
{
    timer->have_start = false;
    timer->armed = timer->interrupted = true;
    timer->exit_seen = false;
}

bool lap_timer_consume(lap_timer_t *timer, const lap_event_t *event, uint64_t *lap_us)
{
    if (!timer || !event || !timer->running || !event->sequence) return false;
    if (!timer->have_session || timer->session != event->session) {
        lap_timer_invalidate(timer);
        timer->session = event->session;
        timer->have_session = true;
        timer->have_sequence = false;
    } else if (timer->have_sequence) {
        /* An old fetch/ACK retry cannot replace the accepted event cursor.
         * Same-boot comparisons require less than half a sequence space. */
        uint32_t distance=event->sequence-timer->last_sequence;
        if(!distance || distance>=UINT32_C(0x80000000))return false;
        if(event->timestamp_us<=timer->last_timestamp_us){
            lap_timer_invalidate(timer);
            return false;
        }
        uint32_t expected_sequence=timer->last_sequence+1u;
        if(!expected_sequence)expected_sequence=1;
        if(event->sequence!=expected_sequence)lap_timer_invalidate(timer);
    }
    timer->last_sequence = event->sequence;
    timer->last_timestamp_us = event->timestamp_us;
    timer->have_sequence = true;
    if (!event->valid || (event->edge != LAP_ENTER && event->edge != LAP_EXIT)) {
        lap_timer_invalidate(timer);
        return false;
    }
    if (event->edge == LAP_EXIT) {
        if (timer->have_start) timer->exit_seen = timer->armed = true;
        return false;
    }
    if (!timer->have_start) {
        timer->start_us = event->timestamp_us;
        timer->have_start = true;
        timer->armed = timer->exit_seen = timer->interrupted = false;
        return false;
    }
    if (!timer->armed || !timer->exit_seen) return false;
    uint64_t elapsed = event->timestamp_us - timer->start_us;
    timer->armed = false;
    if (elapsed < timer->minimum_lap_us) return false;
    timer->start_us = event->timestamp_us;
    timer->exit_seen = false;
    ++timer->total_laps;
    timer->recent_us[timer->recent_next] = elapsed;
    timer->recent_next = (timer->recent_next + 1u) % LAP_RECENT_MAX;
    if (timer->recent_count < LAP_RECENT_MAX) ++timer->recent_count;
    if (!timer->fastest_us || elapsed < timer->fastest_us) {
        timer->fastest_us = elapsed;
        timer->fastest_lap_number = timer->total_laps;
    }
    if (lap_us) *lap_us = elapsed;
    return true;
}
