#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_scan.h"
typedef struct {unsigned calls,fail;uint8_t state;} fake_t;
static int32_t exchange(void *ctx,unsigned slot,uint8_t cmd,const uint8_t *in,uint8_t len,uint8_t expected,uint8_t *out){
    fake_t *f=ctx;assert(slot==0 && !in && !len);f->calls++;
    if(f->calls==f->fail)return 263;
    if(f->calls==1){assert(cmd==17 && !expected && !out);}
    else {assert(f->calls==2 && cmd==16 && expected==12);memset(out,0,12);out[0]=f->state;}
    return 0;
}
int main(void){
    for(unsigned state=0;state<256;state++){
        fake_t f={.state=state};int32_t e=receiver_scan_stop(exchange,&f,0,259,264);
        assert(f.calls==2);
        assert(e==(state==1?259:state>4?264:0));
        receiver_scan_stop_t p={0};bool done=true;f=(fake_t){.state=state};
        assert(!receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done) && !done && f.calls==1 && p.status_due);
        e=receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done);
        assert(f.calls==2 && e==(state==1?259:state>4?264:0));
        assert(done==(state!=1 && state<=4) && !p.status_due);
    }
    for(unsigned fail=1;fail<=2;fail++){
        fake_t f={.fail=fail};assert(receiver_scan_stop(exchange,&f,0,259,264)==263);assert(f.calls==fail);
        f=(fake_t){.state=1};assert(receiver_scan_stop(exchange,&f,0,259,264)==259);
        f=(fake_t){.state=3};assert(!receiver_scan_stop(exchange,&f,0,259,264));
        receiver_scan_stop_t p={0};bool done=false;f=(fake_t){.fail=fail};
        int32_t e=receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done);
        if(!e)e=receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done);
        assert(e==263 && !done && !p.status_due);
        f=(fake_t){.state=3};assert(!receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done) && !done);
        assert(!receiver_scan_stop_step(&p,exchange,&f,0,259,264,&done) && done);
    }
    uint8_t s[12]={1,0,64,0,0,0,0,0,5};uint32_t generation=0;
    assert(receiver_scan_status(s,&generation) && generation==5);
    s[8]=6;assert(!receiver_scan_status(s,&generation));assert(generation==5);
    s[8]=5;s[0]=2;assert(!receiver_scan_status(s,&generation));
    s[1]=64;assert(receiver_scan_status(s,&generation));
    s[1]=65;assert(!receiver_scan_status(s,&generation));
    s[1]=64;s[2]=63;assert(!receiver_scan_status(s,&generation));
    s[2]=64;s[8]=0;assert(!receiver_scan_status(s,&generation));
    puts("PASS cancel ACK/status loss, active retry, all state bytes, generation changes and partial results");
}
