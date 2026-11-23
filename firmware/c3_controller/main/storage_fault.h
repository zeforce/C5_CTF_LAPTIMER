#pragma once
#include <stdbool.h>
#include <stdint.h>
enum {STORAGE_FAULT_NONE=0,STORAGE_FAULT_GAME_CONFIG=1,STORAGE_FAULT_GAME_RESULT=2,STORAGE_FAULT_NETWORK_RESULT=3};
/* Serial-only one-shot pre-write failure; expires after 30 seconds. */
void storage_fault_arm(uint8_t target);
bool storage_fault_take(uint8_t target);
