#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/ble_connection.h"
#include "../firmware/c3_controller/main/config_transfer.h"
#include "../firmware/c3_controller/main/network_provision.h"
int main(void){
 assert(ble_connection_accept(C3_BLE_NO_CONNECTION,0));
 assert(ble_connection_accept(7,7));assert(!ble_connection_accept(7,8));
 assert(!ble_connection_accept(C3_BLE_NO_CONNECTION,C3_BLE_NO_CONNECTION));
 assert(ble_connection_matches(7,7));assert(!ble_connection_matches(7,8));
 assert(!ble_connection_matches(C3_BLE_NO_CONNECTION,C3_BLE_NO_CONNECTION));
 /* Old disconnect/subscribe cannot clear or enable another client. */
 uint16_t current=8;if(ble_connection_matches(current,7))current=C3_BLE_NO_CONNECTION;
 assert(current==8);if(ble_connection_matches(current,8))current=C3_BLE_NO_CONNECTION;
 assert(ble_connection_accept(current,9));
 uint32_t generation=0;
 assert(!ble_transaction_boundary(&generation,0));
 assert(ble_transaction_boundary(&generation,1));
 assert(!ble_transaction_boundary(&generation,1));
 assert(ble_transaction_boundary(&generation,0));assert(generation==0);
 assert(!ble_transaction_boundary(&generation,0));
 assert(ble_transaction_boundary(&generation,3));assert(generation==3);
 assert(ble_transaction_boundary(&generation,1));
 /* A new generation discards incomplete staging; same generation retains it. */
 config_transfer_t transfer={0};uint8_t buffer[4],part[2]={1,2};generation=0;
 if(ble_transaction_boundary(&generation,1))config_transfer_reset(&transfer);
 assert(config_transfer_chunk(&transfer,buffer,4,0,part,2,1));
 if(ble_transaction_boundary(&generation,1))config_transfer_reset(&transfer);
 assert(transfer.received==2);
 if(ble_transaction_boundary(&generation,3))config_transfer_reset(&transfer);
 assert(!config_transfer_chunk(&transfer,buffer,4,2,part,2,2));
 assert(config_transfer_chunk(&transfer,buffer,4,0,part,2,3));
 /* A serial command must invalidate BLE staging, even before BLE returns. */
 if(ble_transaction_boundary(&generation,0))config_transfer_reset(&transfer);
 assert(!config_transfer_chunk(&transfer,buffer,4,2,part,2,4));
 assert(config_transfer_chunk(&transfer,buffer,4,0,part,2,5));
 if(ble_transaction_boundary(&generation,3))config_transfer_reset(&transfer);
 assert(!config_transfer_chunk(&transfer,buffer,4,2,part,2,6));
 /* The same source boundary must discard a complete provisioning draft,
  * so the other client cannot commit a configuration it did not stage. */
 network_provision_t provision={0};network_config_t target={0};uint8_t key[8]={1};
 assert(network_provision_begin(&provision,NET_HOST,1,513,10));
 assert(network_provision_key(&provision,0,key,11));
 assert(network_provision_key(&provision,1,key,12));
 if(ble_transaction_boundary(&generation,0))network_provision_reset(&provision);
 assert(!network_provision_take(&provision,&target,13));
 assert(network_provision_begin(&provision,NET_HOST,1,513,14));
 assert(network_provision_key(&provision,0,key,15));
 assert(network_provision_key(&provision,1,key,16));
 if(ble_transaction_boundary(&generation,3))network_provision_reset(&provision);
 assert(!network_provision_take(&provision,&target,17));
 for(unsigned flags=0;flags<4;flags++){
  assert(ble_command_admitted(7,7,flags&1,flags&2)==(flags==1));
  assert(!ble_command_admitted(6,7,flags&1,flags&2));
  assert(!ble_command_admitted(0,0,flags&1,flags&2));
 }
 /* A job queued before revocation stays invalid after successful re-pairing. */
 assert(ble_command_admitted(7,7,true,false));
 assert(!ble_command_admitted(7,8,false,false));
 assert(!ble_command_admitted(7,8,true,false));
 assert(ble_command_admitted(8,8,true,false));
 assert(ble_result_current(7,7));assert(!ble_result_current(7,8));
 assert(!ble_result_current(0,0));assert(!ble_result_current(0,7));
 /* Same request ID in a new connection cannot deduplicate against old result. */
 unsigned request_id=42,old_id=42;
 assert(ble_result_current(7,7) && request_id==old_id);
 assert(!(ble_result_current(7,8) && request_id==old_id));
 /* Worker initially admits, but disconnection precedes transaction admission. */
 uint32_t queued=8,active=8;bool authorized=true,failed=false;
 assert(ble_command_admitted(queued,active,authorized,failed));
 active=9;authorized=false;
 assert(!ble_command_admitted(queued,active,authorized,failed));
 authorized=true;assert(!ble_command_admitted(queued,active,authorized,failed));
 puts("PASS single-client acceptance, unrelated/stale event rejection and reconnect acceptance");
}
