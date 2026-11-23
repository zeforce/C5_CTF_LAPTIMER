#include "storage_fault.h"
#include "storage_fault_gate.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static storage_fault_gate_t gate;
void storage_fault_arm(uint8_t target){taskENTER_CRITICAL(&lock);storage_fault_gate_arm(&gate,target,esp_timer_get_time());taskEXIT_CRITICAL(&lock);}
bool storage_fault_take(uint8_t target){taskENTER_CRITICAL(&lock);bool fail=storage_fault_gate_take(&gate,target,esp_timer_get_time());taskEXIT_CRITICAL(&lock);return fail;}
