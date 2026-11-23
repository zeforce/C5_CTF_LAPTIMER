#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {
 bool initialized,valid; uint16_t power,reference; uint8_t level,trend;
 uint64_t sample_us,reference_us;
} finder_t;
/* Trend: 0 steady, 1 stronger, 2 weaker. Smooth once per real sample. */
static inline void finder_update(finder_t *f,bool valid,uint16_t power,uint64_t sample_us){
 if(!valid){memset(f,0,sizeof(*f));return;}
 if(f->initialized && f->sample_us==sample_us)return;
 f->sample_us=sample_us;f->valid=true;
 if(!f->initialized){f->power=f->reference=power;f->reference_us=sample_us;f->initialized=true;}
 else f->power=((uint32_t)f->power*3+power)/4;
 if(sample_us-f->reference_us>=1000000){
  unsigned margin=f->reference/10;if(margin<2)margin=2;
  f->trend=f->power>(unsigned)f->reference+margin?1:(unsigned)f->power+margin<f->reference?2:0;
  f->reference=f->power;f->reference_us=sample_us;
 }
 /* Logarithmic scale keeps both weak and strong signals useful. */
 unsigned p=f->power;f->level=0;while(p && f->level<10){++f->level;p>>=1;}
}
static inline bool finder_pulse(const finder_t *f,uint64_t now_ms){
 return f->valid && f->power && now_ms%(1200-f->level*100)<60;
}
