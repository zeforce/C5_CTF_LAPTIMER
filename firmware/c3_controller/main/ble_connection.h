#pragma once
#include <stdbool.h>
#include <stdint.h>
#define C3_BLE_NO_CONNECTION UINT16_MAX
/* GAP callbacks are serialized by the NimBLE host. Reject unrelated handles
 * rather than allowing a second or stale event to replace the active client. */
static inline bool ble_connection_accept(uint16_t current,uint16_t incoming){
 return incoming!=C3_BLE_NO_CONNECTION && (current==C3_BLE_NO_CONNECTION || current==incoming);
}
static inline bool ble_connection_matches(uint16_t current,uint16_t incoming){
 return current!=C3_BLE_NO_CONNECTION && current==incoming;
}

/* Called only under the network transaction lock. Generation zero identifies
 * serial control. Switching either direction must discard staged fragments. */
static inline bool ble_transaction_boundary(uint32_t *previous,uint32_t generation){
 if(*previous==generation)return false;
 *previous=generation;return true;
}

/* Recheck immediately before the worker admits a queued command. */
static inline bool ble_command_admitted(uint32_t queued,uint32_t current,bool authorized,bool store_failed){
 return queued!=0 && queued==current && authorized && !store_failed;
}

static inline bool ble_result_current(uint32_t cached,uint32_t current){return cached!=0 && cached==current;}
