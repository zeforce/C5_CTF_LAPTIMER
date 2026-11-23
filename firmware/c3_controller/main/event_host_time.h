#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
/* Offset and uncertainty are reported by the member's authenticated four-time
 * exchange. 1000ppm is a budget assumption, not a measured accuracy guarantee. */
typedef struct {uint32_t member_boot,host_boot,uncertainty_us;uint64_t sampled_us;int64_t offset_us;bool valid;} event_host_clock_t;
typedef struct {uint64_t host_us;uint32_t uncertainty_us,host_boot;uint8_t reason;} event_host_time_t;
/* reason: 0 mapped, 1 source unmapped/regressed, 2 absent/boot mismatch,
 * 3 stale/outside horizon, 4 arithmetic/quality budget. */
static inline bool event_host_add(uint64_t value,int64_t offset,uint64_t *out){
 if(offset>=0){if(value>UINT64_MAX-(uint64_t)offset)return false;*out=value+(uint64_t)offset;}
 else {uint64_t magnitude=(uint64_t)(-(offset+1))+1;if(value<magnitude)return false;*out=value-magnitude;}return true;
}
static inline event_host_time_t event_host_map(const uint8_t payload[32],const event_host_clock_t *clock,uint32_t member_boot,uint32_t host_boot,uint64_t now,bool local){
 event_host_time_t r={.host_boot=host_boot,.reason=1};uint64_t node;uint32_t source_error;
 memcpy(&node,payload+16,8);memcpy(&source_error,payload+24,4);
 if((payload[29]&3)!=3 || (payload[29]&8) || !node)return r;
 if(local){r.host_us=node;r.uncertainty_us=source_error;r.reason=0;return r;}
 r.reason=2;if(!clock || !clock->valid || clock->member_boot!=member_boot || clock->host_boot!=host_boot)return r;
 uint64_t sampled_host,mapped;
 r.reason=4;if(!event_host_add(clock->sampled_us,clock->offset_us,&sampled_host) || !event_host_add(node,clock->offset_us,&mapped))return r;
 uint64_t distance=node>clock->sampled_us?node-clock->sampled_us:clock->sampled_us-node;
 r.reason=3;if(now<sampled_host || now-sampled_host>10000000 || distance>10000000)return r;
 r.reason=4;if(clock->uncertainty_us>20000)return r;
 uint64_t uncertainty=(uint64_t)source_error+clock->uncertainty_us+(distance+999)/1000;
 if(uncertainty>UINT32_MAX || mapped>now+uncertainty)return r;
 r.host_us=mapped;r.uncertainty_us=uncertainty;r.reason=0;return r;
}
