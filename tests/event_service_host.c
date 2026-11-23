#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/c3_controller/main/event_service.h"
int main(void){event_service_t s={0};service_event_t e={.session=1,.sequence=UINT32_MAX,.source_us=100,.edge=1,.flags=1},v;uint8_t p[32];
 node_time_t m={0};assert(node_time_sample(&m,1,100,1000,1200));
 assert(event_service_push(&s,&e,&m,1200));assert(event_service_get(&s,0,&v) && v.flags==3 && v.local_us==1100);
 assert(!event_service_push(&s,&e,&m,1200) && s.duplicates==1);
 e.sequence=1;e.source_us=200;assert(event_service_push(&s,&e,&m,1200) && !s.gaps);
 e.sequence=UINT32_MAX;assert(!event_service_push(&s,&e,&m,1200) && s.stale==1);
 e.sequence=3;e.source_us=150;assert(event_service_push(&s,&e,&m,1200) && s.gaps==1 && s.regressions==1);
 assert(event_service_get(&s,0,&v) && (v.flags&12)==12);
 for(unsigned n=4;n<44;n++){e.sequence=n;e.source_us=100*n;assert(event_service_push(&s,&e,0,1200));}
 assert(s.count==32 && s.accepted==43 && s.overwritten==11);
 assert(event_service_get(&s,31,&v) && v.sequence==12);assert(!event_service_get(&s,32,&v));
 e.session=2;e.sequence=1;e.source_us=1;assert(event_service_push(&s,&e,&m,1200));assert(event_service_get(&s,0,&v) && v.flags==1 && v.uncertainty_us==UINT32_MAX);
 event_service_page(&s,38,p);assert(p[2]==32 && p[3]==32);event_service_page(&s,39,p);assert(p[2]==1 && p[3]==1 && p[4]==2);
 event_snapshot_t snapshot={0};assert(event_snapshot_page(&snapshot,71,2000,p) && !p[2]);
 event_snapshot_capture(&snapshot,&s,2000);assert(snapshot.revision==1);
 uint8_t frozen[32];assert(event_snapshot_page(&snapshot,73,3000,frozen));assert(frozen[1]==73 && frozen[4]==2);
 e.sequence=2;e.source_us=2;event_service_push(&s,&e,0,3000);
 assert(event_snapshot_page(&snapshot,73,4000,p) && !memcmp(frozen,p,32));
 assert(event_snapshot_page(&snapshot,71,5000,p) && p[2] && p[3]==32 && p[8]==3);
 event_snapshot_capture(&snapshot,&s,6000);assert(snapshot.revision==2);event_snapshot_page(&snapshot,73,7000,p);assert(p[8]==2);
 snapshot.revision=UINT32_MAX;event_snapshot_capture(&snapshot,&s,8000);assert(snapshot.revision==1);
 assert(!event_snapshot_page(&snapshot,105,9000,p));
 /* Consumers do not steal events from each other. Retry before commit
  * returns the same item, slow consumer observes exact overwrite loss. */
 event_service_t journal={0};uint64_t fast=0,slow=0;event_delivery_t d;
 for(unsigned n=1;n<=100;n++){
  e.session=n<50?7:8;e.sequence=n;e.source_us=n;
  assert(event_service_push(&journal,&e,0,n));
  assert(event_service_peek(&journal,fast,&v,&d) && v.sequence==n && !d.lost && !d.reset);
  event_delivery_t retry;service_event_t again;assert(event_service_peek(&journal,fast,&again,&retry));
  assert(retry.position==d.position && again.sequence==v.sequence);
  fast=d.position;assert(!event_service_peek(&journal,fast,&v,&d));
 }
 assert(event_service_peek(&journal,slow,&v,&d) && d.lost==68 && v.sequence==69);slow=d.position;
 for(unsigned n=70;n<=100;n++){assert(event_service_peek(&journal,slow,&v,&d) && !d.lost && v.sequence==n);slow=d.position;}
 assert(slow==fast && !event_service_peek(&journal,slow,&v,&d));
 assert(event_service_peek(&journal,1000,&v,&d) && d.reset && v.sequence==69);
 journal.published=UINT32_MAX;for(unsigned n=101;n<104;n++){e.sequence=n;e.source_us=n;event_service_push(&journal,&e,0,n);}
 assert(event_service_peek(&journal,UINT32_MAX,&v,&d) && d.position==(uint64_t)UINT32_MAX+1 && v.sequence==101);
 assert(!event_service_peek(0,0,&v,&d) && !event_service_peek(&journal,0,0,&d));
 puts("PASS bounded event history, overwrite, duplicates/stale/gaps/regression/wrap, source session and time map quality");}
