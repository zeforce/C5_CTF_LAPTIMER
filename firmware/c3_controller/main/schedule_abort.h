#pragma once
#include "network_sequence.h"
/* Session IDs persist across pause/resume. The fixed host sequence fence
 * identifies the cancelled attempt; later prepares survive repeated notices. */
static inline bool schedule_abort_matches(uint32_t cancelled,uint32_t fence,
                                          uint32_t session,uint32_t prepared){
    return cancelled && cancelled==session && fence && prepared &&
           !network_sequence_after(prepared,fence);
}
