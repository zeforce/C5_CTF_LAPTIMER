#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static inline int ble_bond_hex(char c){
 if(c>='0' && c<='9')return c-'0';
 if(c>='a' && c<='f')return c-'a'+10;
 if(c>='A' && c<='F')return c-'A'+10;
 return -1;
}
/* Exact identity type and display-order address, never an unstable list index. */
static inline bool ble_bond_parse(const char *text,uint8_t *type,uint8_t address[6]){
 if(!text || strlen(text)!=19 || (text[0]!='0' && text[0]!='1') || text[1]!=' ')return false;
 uint8_t parsed[6];
 for(unsigned i=0;i<6;i++){
  unsigned at=2+3*i;int hi=ble_bond_hex(text[at]),lo=ble_bond_hex(text[at+1]);
  if(hi<0 || lo<0 || (i<5 && text[at+2]!=':'))return false;
  parsed[5-i]=(hi<<4)|lo;
 }
 *type=text[0]-'0';memcpy(address,parsed,6);return true;
}
static inline bool ble_bond_delete_allowed(unsigned state,bool connected){return !connected && (state==0 || state==3);}
