#include "result_record.h"
#include <stddef.h>
#include <string.h>
static uint32_t checksum(const result_record_t *record){
 uint32_t value=2166136261u;const uint8_t *bytes=(const uint8_t*)record;
 for(unsigned i=0;i<offsetof(result_record_t,checksum);i++)value=(value^bytes[i])*16777619u;
 return value;
}
void result_record_seal(result_record_t *record){record->version=3;record->checksum=checksum(record);}
uint16_t result_record_missing(const result_record_t *record){
 if(!record)return 0;
 uint16_t expected;memcpy(&expected,record->pages[0]+8,2);
 uint16_t missing=expected&~record->outcome_mask;
 for(unsigned i=0;i<RESULT_PAGES;i++){uint16_t page;memcpy(&page,record->pages[i]+12,2);missing|=page;}
 return missing;
}
bool result_record_valid(const result_record_t *record){
 if(!record || record->version!=3 || !record->revision || record->checksum!=checksum(record))return false;
 uint32_t session;memcpy(&session,record->pages[0]+4,4);if(!session)return false;
 for(unsigned i=0;i<8;i++){
  const uint8_t *id=record->pilot_identity[i];
  if(id[0]>1 || id[1]>=64 || id[2]>2 || id[3]>=4 || !memchr(id+7,0,17))return false;
 }

 uint16_t expected;memcpy(&expected,record->pages[0]+8,2);
 if(record->outcome_mask&~expected)return false;
 for(unsigned i=0;i<9;i++)if(record->outcome_mask&(1u<<i)){
  const uint8_t *o=record->node_outcome[i];
  if(o[0]!=3 || o[1]!=record->pages[0][2] || o[2]>8 || o[4]>1 || o[5]>1 || o[6]<1 || o[6]>3 || o[7])return false;
  if(o[1]<4 && o[2]>2)return false;
  if(o[1]!=3 && (o[4] || o[5] || o[6]==3))return false;
  if(o[5] && (!o[4] || o[6]!=3 || o[2]))return false;
 }
 for(unsigned i=0;i<RESULT_PAGES;i++){
  const uint8_t *p=record->pages[i];
  if(p[0]!=4 || p[1]!=i || p[2]>=6 || memcmp(p+4,record->pages[0]+4,4) || p[2]!=record->pages[0][2])return false;
  uint16_t expected,included,missing;memcpy(&expected,p+8,2);memcpy(&included,p+10,2);memcpy(&missing,p+12,2);
  if(!expected || expected&~511u || included&~expected || missing!=(expected&~included) || memcmp(p+8,record->pages[0]+8,2))return false;
 }
 return true;
}

bool result_history_valid(const result_history_t *h){
 if(!h || h->version!=1 || h->count>RESULT_HISTORY_MAX)return false;
 for(unsigned i=0;i<h->count;i++){
  if(!result_record_valid(&h->records[i]))return false;
  for(unsigned j=0;j<i;j++)if(h->records[i].network==h->records[j].network && !memcmp(h->records[i].pages[0]+4,h->records[j].pages[0]+4,4))return false;
 }
 return true;
}
bool result_history_push(result_history_t *h,const result_record_t *r){
 if(!result_history_valid(h) || !result_record_valid(r))return false;
 unsigned index=h->count;
 for(unsigned i=0;i<h->count;i++)if(h->records[i].network==r->network && !memcmp(h->records[i].pages[0]+4,r->pages[0]+4,4)){index=i;break;}
 if(index==h->count){if(h->count<RESULT_HISTORY_MAX)++h->count;index=h->count-1;}
 for(unsigned i=index;i>0;i--)h->records[i]=h->records[i-1];
 h->records[0]=*r;return true;
}

bool result_record_identity_page(const result_record_t *r,unsigned part,uint8_t out[32]){
 if(!r || !out || part>=11 || !r->revision)return false;
 memset(out,0,32);out[0]=15;out[1]=part;memcpy(out+4,&r->revision,4);
 if(part<8)memcpy(out+8,r->pilot_identity[part],24);
 else {unsigned first=(part-8)*4,count=9-first;if(count>4)count=4;out[2]=first;out[3]=count;memcpy(out+8,r->node_mac[first],count*6);}
 return true;
}

bool result_record_outcome_page(const result_record_t *r,unsigned node,uint8_t out[32]){
 if(!r || !out || node>=9 || !r->revision)return false;
 memset(out,0,32);out[0]=16;out[1]=node;out[2]=(r->outcome_mask>>node)&1;
 memcpy(out+4,&r->revision,4);if(out[2])memcpy(out+8,r->node_outcome[node],24);return true;
}
