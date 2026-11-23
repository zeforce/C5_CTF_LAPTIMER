#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Physical admission window for new bonds; existing bonds still prove keys. */
#define BLE_AUTH_WINDOW_US INT64_C(120000000)
typedef struct { int64_t opened_us; bool open; } ble_authorization_t;
static inline void ble_authorization_open(ble_authorization_t *s,int64_t now){s->opened_us=now;s->open=true;}
static inline bool ble_authorization_available(const ble_authorization_t *s,int64_t now){
 return s->open && now>=s->opened_us && now-s->opened_us<BLE_AUTH_WINDOW_US;
}
static inline bool ble_authorization_take(ble_authorization_t *s,int64_t now){
 bool admitted=ble_authorization_available(s,now);s->open=false;return admitted;
}

static inline bool ble_authorization_secure(bool admitted,bool encrypted,bool bonded,unsigned key_size){
 return admitted && encrypted && bonded && key_size==16;
}

/* Replacing keys requires a distinct physical authorization and an idle game. */
static inline bool ble_authorization_repair(ble_authorization_t *window,int64_t now,bool current,unsigned state){
 if(!current || (state!=0 && state!=3))return false;
 return ble_authorization_take(window,now);
}

static inline bool ble_authorization_storage_ok(bool failed,bool secure){return !failed && secure;}

#define BLE_AUTH_HANDSHAKE_US INT64_C(30000000)
static inline bool ble_authorization_expired(int64_t started,int64_t now,bool authorized){
 return !authorized && (now<started || now-started>=BLE_AUTH_HANDSHAKE_US);
}
