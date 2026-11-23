#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/event_mux.h"
int main(void){
 event_service_t journals[4]={0};event_mux_t mux={0};event_sender_t sender={0};event_receiver_t rx={0};uint8_t payload[32];
 for(unsigned slot=0;slot<4;slot++)for(unsigned seq=1;seq<=3;seq++){
  service_event_t e={.session=1,.sequence=seq,.source_us=seq*100,.edge=1,.flags=1};assert(event_service_push(&journals[slot],&e,NULL,1000));
 }
 event_sender_bind(&sender,11);unsigned delivered[4]={0};uint32_t wire_seq=0;
 for(unsigned i=0;i<12;i++){
  unsigned slot=event_mux_next(&mux);service_event_t e;event_delivery_t d;assert(event_service_peek(&journals[slot],mux.cursor[slot],&e,&d));
  assert(event_mux_prepare(&mux,&sender,slot,++wire_seq,&e,&d,payload));assert(event_transport_slot(payload)==slot);assert(event_sender_due(&sender,1000000*i));
  assert(event_transport_receive(&rx,5,wire_seq,payload)==1);
  if(i==0){
   uint8_t copy[32];memcpy(copy,payload,32);assert(!event_mux_prepare(&mux,&sender,1,999,&e,&d,payload));assert(!memcmp(copy,payload,32));
   assert(!event_mux_ack(&mux,&sender,12,5,5,wire_seq));event_sender_bind(&sender,12);assert(!event_mux_ack(&mux,&sender,11,5,5,wire_seq));
   assert(event_sender_due(&sender,250000));assert(event_transport_receive(&rx,5,wire_seq,payload)==0);assert(event_mux_ack(&mux,&sender,12,5,5,wire_seq));event_sender_bind(&sender,11);
  }else assert(event_mux_ack(&mux,&sender,11,5,5,wire_seq));
  ++delivered[slot];assert(mux.cursor[slot]==delivered[slot]);for(unsigned j=0;j<4;j++)assert(mux.cursor[j]==delivered[j]);
 }
 assert(rx.accepted==12 && rx.duplicates==1);for(unsigned i=0;i<4;i++)assert(delivered[i]==3);
 /* One slot overruns while other slots retain independent positions. */
 for(unsigned seq=4;seq<=100;seq++){service_event_t e={.session=1,.sequence=seq,.source_us=seq*100,.edge=1,.flags=1};assert(event_service_push(&journals[2],&e,NULL,100000));}
 service_event_t e;event_delivery_t d;assert(event_service_peek(&journals[2],mux.cursor[2],&e,&d));assert(d.lost==65 && d.position==69);
 assert(event_mux_prepare(&mux,&sender,2,++wire_seq,&e,&d,payload));assert(payload[30]==65);assert(event_mux_ack(&mux,&sender,11,5,5,wire_seq));assert(mux.cursor[2]==69 && mux.cursor[0]==3 && mux.cursor[1]==3 && mux.cursor[3]==3);
 puts("PASS four journals with equal sessions/positions, round robin, dropped/wrong/reboot ACK, immutable pending and isolated overflow");
}
