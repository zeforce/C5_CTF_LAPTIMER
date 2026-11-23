#pragma once
#include <stdint.h>
#include <limits.h>
/* Commands that can change RF ownership/configuration, even if their reply is
 * lost. Read-only traffic and event ACKs must not invalidate a health sweep. */
static inline int node_spi_mutates(uint8_t command){
    return command==4 || command==8 || command==9 || command==14 || command==15 || command==17 || command==20;
}
/* Ordinary exchanges share one READY budget across request and NOP frames.
 * Legacy command9 performs eight RF measurements synchronously. */
static inline int64_t node_spi_budget_us(uint8_t command){
    return command==9?INT64_C(2000000):INT64_C(100000);
}
static inline int64_t node_spi_default_deadline(uint8_t command,int64_t now){
    int64_t budget=node_spi_budget_us(command);
    return now>INT64_MAX-budget?INT64_MAX:now+budget;
}
