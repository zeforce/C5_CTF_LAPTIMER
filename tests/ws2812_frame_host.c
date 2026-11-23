#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/ws2812_frame.h"
int main(void){
 uint8_t b[3];
 ws2812_grb(0x123456,255,true,b);assert(b[0]==0x34 && b[1]==0x12 && b[2]==0x56);
 ws2812_grb(0xff8000,32,true,b);assert(b[0]==16 && b[1]==32 && b[2]==0);
 ws2812_grb(0xffffff,0,true,b);assert(!(b[0]|b[1]|b[2]));
 ws2812_grb(0xffffff,255,false,b);assert(!(b[0]|b[1]|b[2]));
 for(unsigned c=0;c<256;c++){ws2812_grb(c|(c<<8)|(c<<16),255,true,b);assert(b[0]==c && b[1]==c && b[2]==c);}
 puts("PASS WS2812 GRB order, brightness, blink-off and full-range identity");
}
