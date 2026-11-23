#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t session;int64_t at_us;uint8_t phase,action;} start_schedule_t;
enum { START_EMPTY=0, START_PREPARED=1, START_ARMED=2 };
bool start_schedule_prepare(start_schedule_t *s,uint32_t session,int64_t local_at,int64_t now);
bool start_schedule_prepare_action(start_schedule_t *s,uint32_t session,int64_t local_at,int64_t now,uint8_t action);
bool start_schedule_arm(start_schedule_t *s,uint32_t session,int64_t now);
bool start_schedule_cancel(start_schedule_t *s,uint32_t session);
/* Due schedules are consumed once. Prepared-only/late schedules never start. */
uint32_t start_schedule_take(start_schedule_t *s,int64_t now,bool healthy);
