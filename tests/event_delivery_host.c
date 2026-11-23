#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/event_transport.h"
int main(void){
 event_service_t journal={0};event_sender_t sender={0};event_receiver_t host={0};
 event_sender_bind(&sender,10);
 service_event_t e={.session=7,.sequence=1,.source_us=100,.edge=1,.flags=1};
 assert(event_service_push(&journal,&e,0,0));
 event_delivery_t d;assert(event_service_peek(&journal,sender.cursor,&e,&d));
 uint8_t payload[32];event_transport_encode(&e,d.lost,payload);
 assert(event_sender_prepare(&sender,100,d.position,payload));
 assert(!event_sender_prepare(&sender,101,d.position+1,payload));
 assert(!event_sender_due(&sender,-1));assert(event_sender_due(&sender,0));
 assert(event_transport_receive(&host,20,sender.sequence,sender.payload)==1);
 /* First ACK is lost; pending data outlives source-ring overwrite. */
 for(unsigned i=2;i<=100;i++){e.sequence=i;e.source_us=i*100;assert(event_service_push(&journal,&e,0,0));}
 assert(!event_sender_due(&sender,249999));assert(event_sender_due(&sender,250000));
 assert(event_transport_receive(&host,20,sender.sequence,sender.payload)==0);
 assert(!event_sender_ack(&sender,9,20,20,100));
 assert(!event_sender_ack(&sender,10,20,21,100));
 assert(!event_sender_ack(&sender,10,20,20,101));assert(sender.cursor==0);
 /* Host reboot binds future ACKs, retains exact in-flight identity/data. */
 event_sender_bind(&sender,11);assert(!event_sender_ack(&sender,10,20,20,100));
 event_receiver_t restarted={0};assert(event_transport_receive(&restarted,20,100,sender.payload)==1);
 assert(event_sender_ack(&sender,11,20,20,100));assert(sender.cursor==1);
 assert(!event_sender_ack(&sender,11,20,20,100));
 assert(event_service_peek(&journal,sender.cursor,&e,&d));assert(d.position==69 && d.lost==67);
 event_transport_encode(&e,d.lost,payload);assert(event_sender_prepare(&sender,101,d.position,payload));
 assert(event_sender_due(&sender,500000));assert(event_transport_receive(&restarted,20,101,sender.payload)==1);
 assert(event_sender_ack(&sender,11,20,20,101));
 while(event_service_peek(&journal,sender.cursor,&e,&d)){
  event_transport_encode(&e,d.lost,payload);uint32_t seq=sender.sequence+1;
  assert(event_sender_prepare(&sender,seq,d.position,payload));assert(event_sender_due(&sender,1000000));
  assert(event_transport_receive(&restarted,20,seq,sender.payload)==1);
  assert(event_sender_ack(&sender,11,20,20,seq));
 }
 assert(sender.cursor==100 && sender.retries==1 && sender.acked==33 && sender.sent==34);
 assert(restarted.accepted==33 && restarted.lost==67 && host.duplicates==1);
 puts("PASS dropped ACK, immutable retry after 100 events/overwrite, host reboot ACK binding, exact lost count and complete remaining drain");
}
