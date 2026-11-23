#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "network_sequence.h"

enum { NETWORK_SCORE_BYTES=21, NETWORK_SCORE_PILOTS=8 };
typedef struct {
    uint32_t session, owned_ms, captures, laps, fastest_ms;
    uint8_t pilot;
} network_score_t;

/* Only authenticated heartbeats establish a peer boot. Telemetry cannot
 * resurrect an expired peer or replace its current boot identity. */
static inline bool network_telemetry_current(uint32_t boot,uint32_t current_boot,
                                            uint64_t heartbeat_us,uint64_t now_us){
    return boot==current_boot && heartbeat_us && now_us>=heartbeat_us &&
           now_us-heartbeat_us<UINT64_C(5000000);
}
static inline bool network_score_decode(const uint8_t *bytes,size_t length,
                                        network_score_t *out){
    if(!bytes || !out || length!=NETWORK_SCORE_BYTES || bytes[4]>=NETWORK_SCORE_PILOTS)return false;
    network_score_t value={.pilot=bytes[4]};
    memcpy(&value.session,bytes,4);memcpy(&value.owned_ms,bytes+5,4);
    memcpy(&value.captures,bytes+9,4);memcpy(&value.laps,bytes+13,4);
    memcpy(&value.fastest_ms,bytes+17,4);*out=value;return true;
}
static inline bool network_score_encode(const network_score_t *value,uint8_t *bytes){
    if(!value || !bytes || value->pilot>=NETWORK_SCORE_PILOTS)return false;
    memcpy(bytes,&value->session,4);bytes[4]=value->pilot;
    memcpy(bytes+5,&value->owned_ms,4);memcpy(bytes+9,&value->captures,4);
    memcpy(bytes+13,&value->laps,4);memcpy(bytes+17,&value->fastest_ms,4);return true;
}
static inline bool network_score_current(const network_score_t *value,
                                         uint32_t session,uint32_t seq,uint32_t previous){
    return value && value->pilot<NETWORK_SCORE_PILOTS &&
           network_report_fresh(seq,previous,value->session==session);
}
