#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/config_transfer.h"
int main(void){
 config_transfer_t t={0};uint8_t buffer[8]={0},data[8]={1,2,3,4,5,6,7,8};uint32_t checksum=2166136261u;
 for(unsigned i=0;i<8;i++)checksum=(checksum^data[i])*16777619u;
 assert(!config_transfer_chunk(&t,buffer,8,4,data,4,1));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,1));
 config_transfer_reset(&t); /* authenticated host boot change */
 assert(!config_transfer_chunk(&t,buffer,8,4,data+4,4,2));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,3));
 assert(!config_transfer_commit(&t,buffer,8,checksum,4));
 assert(!config_transfer_chunk(&t,buffer,8,4,data+4,4,5));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,6));
 assert(config_transfer_chunk(&t,buffer,8,4,data+4,4,7));
 assert(config_transfer_commit(&t,buffer,8,checksum,8));assert(!memcmp(buffer,data,8));
 assert(!config_transfer_commit(&t,buffer,8,checksum,9));
 assert(config_transfer_chunk(&t,buffer,8,0,data,8,10));
 assert(!config_transfer_commit(&t,buffer,8,checksum+1,11));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,10));
 assert(!config_transfer_chunk(&t,buffer,8,4,data+4,4,120000010));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,20));
 assert(!config_transfer_chunk(&t,buffer,8,SIZE_MAX,data,4,21));
 assert(config_transfer_chunk(&t,buffer,8,0,data,4,30));
 assert(!config_transfer_chunk(&t,buffer,8,4,data,5,31));
 puts("PASS host reboot reset, ordered full commit, incomplete/checksum/expiry/overflow rejection and one-shot commit");
}
