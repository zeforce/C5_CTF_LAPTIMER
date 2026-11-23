#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/event_transport.h"
int main(void){event_receiver_t r={0};service_event_t e={.session=1,.sequence=5,.source_us=100,.edge=1,.flags=1};uint8_t p[32];
 event_transport_encode(&e,3,p);assert(event_transport_receive(&r,4,UINT32_MAX,p)==1 && r.lost==3);
 assert(event_transport_receive(&r,4,UINT32_MAX,p)==0 && r.accepted==1 && r.duplicates==1 && r.lost==3);
 p[8]++;assert(event_transport_receive(&r,4,UINT32_MAX,p)==-1);p[8]--;
 assert(event_transport_receive(&r,4,UINT32_MAX-1,p)==-1);
 e.sequence=6;event_transport_encode(&e,0,p);assert(event_transport_receive(&r,4,1,p)==1);
 assert(event_transport_receive(&r,5,1,p)==1);
 p[28]=3;assert(event_transport_receive(&r,5,2,p)==-1);p[28]=1;
 p[29]=64;assert(event_transport_receive(&r,5,2,p)==-1);
 event_transport_encode(&e,UINT64_MAX,p);assert(p[30]==255 && p[31]==255);
 for(unsigned slot=0;slot<4;slot++){assert(event_transport_encode_slot(&e,0,slot,p));assert(event_transport_slot(p)==slot && (p[29]&15)==e.flags);assert(event_transport_receive(&r,6,slot+1,p)==1);assert(event_transport_receive(&r,6,slot+1,p)==0);}
 assert(!event_transport_encode_slot(&e,0,4,p));
 puts("PASS transport codec, ACK-eligible exact duplicate, conflicting retry rejection, wrap, sender boot and loss saturation");}
