#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {size_t received;int64_t deadline;} config_transfer_t;
static inline void config_transfer_reset(config_transfer_t *t){t->received=0;t->deadline=0;}
static inline bool config_transfer_chunk(config_transfer_t *t,void *buffer,size_t capacity,
                                         size_t offset,const void *data,size_t size,int64_t now){
    if(!t || !buffer || !data || !size || now<0)return false;
    if(!offset){config_transfer_reset(t);t->deadline=now<=INT64_MAX-120000000?now+120000000:INT64_MAX;}
    if(!t->deadline || now>=t->deadline || offset!=t->received || offset>capacity || size>capacity-offset){config_transfer_reset(t);return false;}
    memcpy((uint8_t*)buffer+offset,data,size);t->received+=size;return true;
}
/* Commit consumes the transaction even on checksum failure. */
static inline bool config_transfer_commit(config_transfer_t *t,const void *buffer,size_t capacity,
                                          uint32_t checksum,int64_t now){
    if(!t || !buffer)return false;
    bool valid=now>=0 && t->deadline && now<t->deadline && t->received==capacity;
    uint32_t actual=2166136261u;
    if(valid)for(size_t i=0;i<capacity;i++)actual=(actual^((const uint8_t*)buffer)[i])*16777619u;
    config_transfer_reset(t);return valid && actual==checksum;
}
