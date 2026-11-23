/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include "../firmware/c3_controller/main/lap_timer.h"

static void feed(lap_timer_t *timer, uint32_t session, uint32_t sequence,
                 uint64_t time, lap_edge_t edge, bool valid, bool expected)
{
    lap_event_t event = {session, sequence, time, edge, valid};
    uint64_t lap = 0;
    assert(lap_timer_consume(timer, &event, &lap) == expected);
}

int main(void)
{
    lap_timer_t timer;
    assert(lap_timer_init(&timer, 1000000u));
    lap_timer_start(&timer);
    feed(&timer, 1, 1, 0, LAP_ENTER, true, false);
    feed(&timer, 1, 2, 100000, LAP_EXIT, true, false);
    feed(&timer, 1, 3, 900000, LAP_ENTER, true, false);
    feed(&timer, 1, 4, 1100000, LAP_ENTER, true, false);
    feed(&timer, 1, 5, 1200000, LAP_EXIT, true, false);
    feed(&timer, 1, 6, 1500000, LAP_ENTER, true, true);
    assert(timer.total_laps == 1 && timer.fastest_us == 1500000);
    feed(&timer, 1, 6, 1500000, LAP_ENTER, true, false);
    feed(&timer, 1, 8, 1600000, LAP_EXIT, true, false); /* gap */
    feed(&timer, 1, 9, 3000000, LAP_ENTER, true, false);
    assert(timer.total_laps == 1);
    feed(&timer, 2, 1, 100, LAP_ENTER, true, false); /* reset */
    feed(&timer, 2, 2, 200, LAP_EXIT, true, false);
    feed(&timer, 2, 3, 1300000, LAP_ENTER, false, false);
    feed(&timer, 2, 4, 1400000, LAP_ENTER, true, false);
    assert(timer.total_laps == 1);
    lap_timer_invalidate(&timer); /* overflow */
    assert(timer.interrupted && !timer.have_start);
    lap_timer_stop(&timer);
    lap_timer_start(&timer);
    uint32_t sequence = 1;
    uint64_t now = 0;
    feed(&timer, 3, sequence++, now, LAP_ENTER, true, false);
    for (unsigned lap = 0; lap < 201; ++lap) {
        feed(&timer, 3, sequence++, now + 100, LAP_EXIT, true, false);
        now += 1000000 + lap;
        feed(&timer, 3, sequence++, now, LAP_ENTER, true, true);
    }
    assert(timer.total_laps == 201 && timer.recent_count == 200);
    assert(timer.fastest_us == 1000000 && timer.fastest_lap_number == 1);
    lap_timer_start(&timer);
    feed(&timer, 4, UINT32_MAX - 1u, 100, LAP_ENTER, true, false);
    feed(&timer, 4, UINT32_MAX, 200, LAP_EXIT, true, false);
    feed(&timer, 4, 1, 1000100, LAP_ENTER, true, true);
    assert(timer.total_laps == 1 && timer.fastest_us == 1000000);
    feed(&timer, 4, 1, 1000100, LAP_ENTER, true, false); /* retry across wrap */
    feed(&timer, 4, 3, 1000200, LAP_EXIT, true, false); /* real gap still invalidates */
    assert(timer.interrupted && !timer.have_start);
    /* Old events must not rewind cursor/start or invent a fresh baseline. */
    lap_timer_start(&timer);
    feed(&timer,5,10,100,LAP_ENTER,true,false);
    feed(&timer,5,11,200,LAP_EXIT,true,false);
    feed(&timer,5,9,50,LAP_ENTER,true,false);
    assert(timer.last_sequence==11 && timer.last_timestamp_us==200 && timer.start_us==100);
    feed(&timer,5,12,1000100,LAP_ENTER,true,true);
    assert(timer.total_laps==1);
    /* Forward sequence with nonmonotonic time invalidates the lap, never
     * moves time backward. A subsequent coherent pair restores measurement. */
    feed(&timer,5,13,1000000,LAP_EXIT,true,false);
    assert(!timer.have_start && timer.last_sequence==12 && timer.last_timestamp_us==1000100);
    feed(&timer,5,14,1100000,LAP_ENTER,true,false);
    feed(&timer,5,15,1200000,LAP_EXIT,true,false);
    feed(&timer,5,16,2100000,LAP_ENTER,true,true);
    assert(timer.total_laps==2 && timer.fastest_us==1000000);
    feed(&timer,5,0,2200000,LAP_EXIT,true,false);
    assert(timer.last_sequence==16);
    /* Reboot replaces both sequence and timestamp cursors. */
    feed(&timer,6,1,1,LAP_ENTER,true,false);
    feed(&timer,6,2,2,LAP_EXIT,true,false);
    feed(&timer,6,3,1000001,LAP_ENTER,true,true);
    assert(timer.total_laps==3);
    /* A complete stream and the same stream with late/duplicate fetches
     * must produce identical lap statistics through multiple ring wraps. */
    lap_timer_t clean,noisy;lap_timer_init(&clean,1000000);lap_timer_init(&noisy,1000000);
    lap_timer_start(&clean);lap_timer_start(&noisy);
    uint32_t seq=UINT32_MAX-50;uint64_t stamp=100;
    for(unsigned n=0;n<2001;n++){
        lap_event_t good={7,seq,stamp,n%2?LAP_EXIT:LAP_ENTER,true};
        bool a=lap_timer_consume(&clean,&good,0),b=lap_timer_consume(&noisy,&good,0);assert(a==b);
        lap_event_t duplicate=good;assert(!lap_timer_consume(&noisy,&duplicate,0));
        if(n>2){lap_event_t old={7,seq-2,stamp-100,n%2?LAP_ENTER:LAP_EXIT,true};if(old.sequence)assert(!lap_timer_consume(&noisy,&old,0));}
        assert(clean.last_sequence==noisy.last_sequence && clean.last_timestamp_us==noisy.last_timestamp_us && clean.total_laps==noisy.total_laps);
        seq++;if(!seq)seq=1;stamp+=600000;
    }
    assert(clean.total_laps==1000 && noisy.total_laps==1000 && noisy.recent_count==200);
    assert(clean.fastest_us==noisy.fastest_us && clean.fastest_lap_number==noisy.fastest_lap_number);
    for(unsigned n=0;n<200;n++)assert(clean.recent_us[n]==noisy.recent_us[n]);
    return 0;
}
