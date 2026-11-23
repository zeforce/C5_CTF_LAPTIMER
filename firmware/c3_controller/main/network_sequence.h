#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Internal command payload ends with the intended receiver boot. */
static inline bool network_command_target(const uint8_t *data,uint8_t length,
 uint32_t local_boot){
 if(!data || length<6 || length>20 || !local_boot)return false;
 uint32_t target;memcpy(&target,data+length-4,4);return target==local_boot;
}

/* Sequence zero is reserved. Comparisons are valid within half the u32 range
 * of one sender boot; boot identity and authentication are checked separately. */
static inline bool network_sequence_after(uint32_t incoming, uint32_t previous) {
    uint32_t distance = incoming - previous;
    return incoming && (!previous || (distance && distance < UINT32_C(0x80000000)));
}

/* Scores from another match never establish liveness. */
static inline bool network_report_fresh(uint32_t incoming, uint32_t previous,
                                        bool identity_matches) {
    return identity_matches && network_sequence_after(incoming, previous);
}

/* Commands may use an established heartbeat identity, never create one.
 * This is a volatile admission fence, not persistent replay protection. */
static inline bool network_command_current(uint32_t incoming_boot,uint32_t current_boot,
    uint64_t heartbeat_us,uint64_t received_us){
    return incoming_boot && incoming_boot==current_boot && heartbeat_us &&
        received_us>=heartbeat_us && received_us-heartbeat_us<UINT64_C(5000000);
}

/* A command cannot be confirmed by a reply retained across a member reboot. */
static inline bool network_reply_current(uint32_t expected_member_boot,
    uint32_t current_member_boot,uint32_t reply_boot,uint32_t host_boot,
    uint32_t reply_host_boot,uint32_t command_seq,uint32_t reply_seq){
    return expected_member_boot && host_boot && command_seq &&
        current_member_boot==expected_member_boot && reply_boot==expected_member_boot &&
        reply_host_boot==host_boot && reply_seq==command_seq;
}
