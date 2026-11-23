/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded UTF-8 scalar validation; rejects overlong, surrogate and >U+10FFFF. */
static inline bool game_name_valid(const char *name,size_t capacity){
 if(!name)return false;
 size_t length=0;while(length<capacity && name[length])++length;
 if(!length || length>12 || length==capacity)return false;
 for(size_t i=0;i<length;){
  uint8_t c=(uint8_t)name[i++];unsigned extra;uint32_t value,minimum;
  if(c<0x80)continue;
  if(c>=0xc2 && c<=0xdf){extra=1;value=c&31;minimum=0x80;}
  else if(c>=0xe0 && c<=0xef){extra=2;value=c&15;minimum=0x800;}
  else if(c>=0xf0 && c<=0xf4){extra=3;value=c&7;minimum=0x10000;}
  else return false;
  if(extra>length-i)return false;
  while(extra--){uint8_t b=(uint8_t)name[i++];if((b&0xc0)!=0x80)return false;value=(value<<6)|(b&63);}
  if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
 }
 return true;
}
