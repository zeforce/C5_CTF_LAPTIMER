#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_events.h"
int main(void){network_events_t j={0};network_events_snapshot_t s={0};uint8_t out[32],payload[32]={0},mac[6]={2,3,4,5,6,7};
 assert(network_events_page(&s,256,0,out) && !out[3]);
 for(unsigned i=1;i<=40;i++){payload[0]=i;mac[5]=i%8;network_events_push(&j,mac,100+i,200+i,payload,1000*i);}
 assert(j.count==32 && j.overwritten==8 && j.published==40);network_events_capture(&s,&j,50000);
 assert(network_events_page(&s,256,51000,out) && out[0]==20 && out[1]==32 && out[4]==1 && out[8]==8 && out[12]==40 && out[24]==1);
 for(unsigned i=0;i<32;i++){unsigned n=40-i;network_events_page(&s,257+i*2,0,out);assert(out[0]==21 && out[1]==i && out[7]==n%8 && out[8]==100+n && out[12]==200+n && out[24]==n);network_events_page(&s,258+i*2,0,out);assert(out[0]==n);}
 payload[0]=99;network_events_push(&j,mac,1,1,payload,60000);network_events_page(&s,258,0,out);assert(out[0]==40);
 network_events_capture(&s,&j,70000);network_events_page(&s,258,0,out);assert(out[0]==99);
 network_event_t *e=&j.ring[(j.next+31)%32];e->time=(event_host_time_t){.host_us=123456,.uncertainty_us=10050,.host_boot=77};network_events_capture(&s,&j,70001);network_events_page(&s,321,71000,out);assert(out[0]==23 && out[2]==1 && out[3]==0 && out[4]==77 && out[24]==41);uint64_t host;memcpy(&host,out+8,8);assert(host==123456);
 e->time.host_us=0;network_events_page(&s,321,71000,out);memcpy(&host,out+8,8);assert(host==123456);
 s.revision=UINT32_MAX;network_events_capture(&s,&j,80000);assert(s.revision==1);
 assert(!network_events_page(&s,353,0,out));puts("PASS multi-node arrival history, 32-slot wrap/loss, immutable snapshot, identities/payload pairs and revision wrap");}
