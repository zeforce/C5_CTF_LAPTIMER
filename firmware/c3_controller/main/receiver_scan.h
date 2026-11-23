#pragma once
#include "receiver_poll.h"
#include "../../common/node_contract.h"
typedef struct {bool status_due;} receiver_scan_stop_t;
/* One request per call; cancellation never completes on ACK alone. */
static inline int32_t receiver_scan_stop_step(receiver_scan_stop_t *p,receiver_exchange_t exchange,void *ctx,
 unsigned slot,int32_t busy,int32_t invalid,bool *done){
 if(done)*done=false;
 if(!p || !exchange || slot>=RECEIVER_SLOTS || !busy || !invalid || !done)return -1;
 int32_t e;
 if(!p->status_due){
  e=exchange(ctx,slot,NODE_CMD_SCAN_CANCEL,NULL,0,0,NULL);
  if(!e)p->status_due=true;
 }else{
  uint8_t status[12]={0};e=exchange(ctx,slot,NODE_CMD_SCAN_STATUS,NULL,0,12,status);
  if(!e){
   if(status[0]==1)e=busy;
   else if(status[0]>4)e=invalid;
   else {*done=true;p->status_due=false;}
  }
 }
 if(e)p->status_due=false;
 return e;
}

/* A cancel ACK only requests cancellation. RF ownership may be released
 * after a recognized idle/terminal status, never an unknown status byte. */
static inline int32_t receiver_scan_stop(receiver_exchange_t exchange,void *ctx,
                                         unsigned slot,int32_t busy,int32_t invalid){
    if(!exchange || slot>=RECEIVER_SLOTS || !busy || !invalid)return -1;
    uint8_t status[12];
    int32_t e=exchange(ctx,slot,NODE_CMD_SCAN_CANCEL,NULL,0,0,NULL);
    if(!e)e=exchange(ctx,slot,NODE_CMD_SCAN_STATUS,NULL,0,12,status);
    if(e)return e;
    if(status[0]==1)return busy;
    if(status[0]!=0 && status[0]!=2 && status[0]!=3 && status[0]!=4)return invalid;
    return 0;
}

/* Validate one full-range scan status before consuming any result pages. */
static inline bool receiver_scan_status(const uint8_t status[12],uint32_t *generation){
    if(!status || !generation)return false;
    uint32_t current=receiver_u32(status+8);
    if(!current || status[2]!=64 || status[1]>64 || status[0]<1 || status[0]>4)return false;
    if(*generation && *generation!=current)return false;
    if(status[0]==2 && status[1]!=64)return false;
    *generation=current;return true;
}
