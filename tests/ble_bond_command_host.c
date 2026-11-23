#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/ble_bond_command.h"
int main(void){
 uint8_t type=9,address[6]={0};
 assert(ble_bond_parse("0 01:23:45:67:89:Ab",&type,address));
 assert(type==0 && address[0]==0xab && address[5]==1);
 assert(ble_bond_parse("1 ff:FF:00:00:00:00",&type,address));assert(type==1);
 const char *bad[]={"","0","2 01:23:45:67:89:ab","0 01:23:45:67:89:ab ","0 01:23:45:67:89:a","0 01-23:45:67:89:ab","0 01:23:45:67:89:gg"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){type=9;assert(!ble_bond_parse(bad[i],&type,address));assert(type==9);}
 assert(!ble_bond_parse(NULL,&type,address));
 for(unsigned state=0;state<5;state++)for(unsigned connection=0;connection<2;connection++)
  assert(ble_bond_delete_allowed(state,connection)==(!connection && (state==0 || state==3)));
 puts("PASS exact bond identity parsing and disconnected idle-only delete policy");
}
