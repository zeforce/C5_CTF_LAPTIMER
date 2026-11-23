#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/c3_controller/main/network_score.h"

int main(void){
    /* Fixed wire fixture exercises nonzero captures/laps and unaligned u32s. */
    uint8_t fixture[21]={0x78,0x56,0x34,0x12,7,0x04,0x03,0x02,0x01,
        0xff,0xff,0xff,0xff,9,0,0,0,0x88,0x13,0,0};
    network_score_t score={0};assert(network_score_decode(fixture,21,&score));
    assert(score.session==0x12345678 && score.pilot==7 && score.owned_ms==0x01020304);
    assert(score.captures==UINT32_MAX && score.laps==9 && score.fastest_ms==5000);
    uint8_t encoded[21];assert(network_score_encode(&score,encoded));assert(!memcmp(encoded,fixture,21));
    network_score_t unchanged=score;
    for(unsigned n=0;n<33;n++)if(n!=21){assert(!network_score_decode(fixture,n,&score));assert(!memcmp(&score,&unchanged,sizeof(score)));}
    fixture[4]=8;assert(!network_score_decode(fixture,21,&score));fixture[4]=255;assert(!network_score_decode(fixture,21,&score));
    assert(!network_score_decode(NULL,21,&score));assert(!network_score_decode(fixture,21,NULL));
    assert(network_score_current(&score,0x12345678,101,100));
    assert(!network_score_current(&score,0x12345679,101,100));
    assert(!network_score_current(&score,score.session,100,100));
    assert(!network_score_current(&score,score.session,99,100));
    assert(!network_score_current(&score,score.session,0,100));
    assert(network_score_current(&score,score.session,1,UINT32_MAX));
    assert(!network_score_current(&score,score.session,UINT32_MAX,1));
    /* Reboot/session/liveness gates precede any score cache mutation. */
    assert(network_telemetry_current(3,3,1000000,5999999));
    assert(!network_telemetry_current(3,3,1000000,6000000));
    assert(!network_telemetry_current(2,3,1000000,1000001));
    assert(!network_telemetry_current(3,3,0,1000001));
    assert(!network_telemetry_current(3,3,1000000,999999));
    puts("PASS score wire fixture, malformed payloads, session/sequence wrap, boot and heartbeat expiry gates");
}
