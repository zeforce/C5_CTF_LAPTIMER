#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Offset-only map. Age error uses an assumed 1000ppm relative drift budget,
 * not a measured oscillator guarantee. Max sample RTT 20ms, max age 3s. */
typedef struct {uint32_t session,rtt_us,samples,rejected;uint64_t sampled_us,node_us;int64_t offset_us;bool valid;} node_time_t;
bool node_time_sample(node_time_t *m,uint32_t session,uint64_t node,uint64_t start,uint64_t end);
bool node_time_map(const node_time_t *m,uint32_t session,uint64_t node,uint64_t now,uint64_t *local,uint32_t *uncertainty);
