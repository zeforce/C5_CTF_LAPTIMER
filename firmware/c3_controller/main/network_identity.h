#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "network_sequence.h"

typedef struct {
 bool used,verified;
 uint8_t mac[6],nonce[16];
 uint32_t boot,floor;
 uint64_t issued;
} network_identity_t;

/* Keep live exchanges, reclaim expired entries so departed MACs cannot fill
 * the admission table forever. The caller owns this table on the RX worker. */
static inline network_identity_t *network_identity_slot(network_identity_t *table,
 unsigned count,const uint8_t mac[6],uint64_t now){
 network_identity_t *available=NULL;
 for(unsigned i=0;i<count;i++){
  network_identity_t *s=&table[i];
  if(s->used && !memcmp(s->mac,mac,6))return s;
  if(!available && (!s->used || now<s->issued || now-s->issued>=3000000))available=s;
 }
 return available;
}

static inline void network_identity_issue(network_identity_t *s,const uint8_t mac[6],
 uint32_t boot,const uint8_t nonce[16],uint64_t now){
 memset(s,0,sizeof(*s));s->used=true;s->boot=boot;s->issued=now;
 memcpy(s->mac,mac,6);memcpy(s->nonce,nonce,16);
}
static inline bool network_identity_accept(network_identity_t *s,uint32_t boot,
 uint32_t sequence,const uint8_t nonce[16],uint64_t now){
 if(!s->used || s->verified || !boot || boot!=s->boot || !sequence ||
    now<s->issued || now-s->issued>=3000000 || memcmp(nonce,s->nonce,16))return false;
 s->verified=true;s->floor=sequence;return true;
}
static inline bool network_identity_admitted(const network_identity_t *s,
 uint32_t boot,uint32_t sequence,uint64_t now){
 return s->used && s->verified && boot==s->boot && now>=s->issued &&
  now-s->issued<3000000 && network_sequence_after(sequence,s->floor);
}
