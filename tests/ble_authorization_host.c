#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/ble_authorization.h"
int main(void){
 ble_authorization_t s={0};assert(!ble_authorization_take(&s,0));
 ble_authorization_open(&s,100);assert(!ble_authorization_available(&s,99));
 assert(ble_authorization_available(&s,100));
 assert(ble_authorization_available(&s,100+BLE_AUTH_WINDOW_US-1));
 assert(!ble_authorization_available(&s,100+BLE_AUTH_WINDOW_US));
 assert(!ble_authorization_take(&s,100+BLE_AUTH_WINDOW_US));
 ble_authorization_open(&s,200);assert(ble_authorization_take(&s,201));assert(!ble_authorization_take(&s,202));
 ble_authorization_open(&s,300);assert(!ble_authorization_take(&s,299));assert(!s.open);
 ble_authorization_open(&s,400);ble_authorization_open(&s,500);assert(ble_authorization_take(&s,500));
 for(unsigned mask=0;mask<8;mask++)for(unsigned size=0;size<=16;size++)
  assert(ble_authorization_secure(mask&1,mask&2,mask&4,size)==(mask==7 && size==16));
 for(unsigned state=0;state<5;state++)for(unsigned current=0;current<2;current++){
  ble_authorization_open(&s,100);
  bool expected=current && (state==0 || state==3);
  assert(ble_authorization_repair(&s,101,current,state)==expected);
  if(expected)assert(!ble_authorization_repair(&s,102,current,state));
  else assert(s.open);
 }
 ble_authorization_open(&s,100);assert(!ble_authorization_repair(&s,100+BLE_AUTH_WINDOW_US,true,0));
 ble_authorization_open(&s,100);assert(!ble_authorization_repair(&s,99,true,0));
 assert(ble_authorization_storage_ok(false,true));
 assert(!ble_authorization_storage_ok(true,true));
 assert(!ble_authorization_storage_ok(true,false));
 assert(!ble_authorization_storage_ok(false,false));
 assert(!ble_authorization_expired(100,100,false));
 assert(!ble_authorization_expired(100,100+BLE_AUTH_HANDSHAKE_US-1,false));
 assert(ble_authorization_expired(100,100+BLE_AUTH_HANDSHAKE_US,false));
 assert(ble_authorization_expired(100,99,false));
 assert(!ble_authorization_expired(100,100+BLE_AUTH_HANDSHAKE_US,true));
 puts("PASS local authorization closed by default, deadline, clock rollback, one-time admission and reopen");
}
