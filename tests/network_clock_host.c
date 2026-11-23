#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_clock.h"
int main(void){
 assert(network_clock_error(0,100,1)==UINT32_MAX);
 assert(network_clock_error(100,99,1)==UINT32_MAX);
 assert(network_clock_error(100,100,5000)==5000);
 assert(network_clock_error(100,101,5000)==5001);
 assert(network_clock_error(100,1000100,5000)==6000);
 assert(network_clock_error(100,10000100,5000)==UINT32_MAX);
 assert(!network_clock_select(100,1000100,5000,60001));
 assert(network_clock_select(100,1000100,5000,5900));
 assert(network_clock_select(100,10000100,5000,60001));
 assert(network_clock_select(0,100,0,5000));
 assert(network_clock_error(100,101,UINT32_MAX)==UINT32_MAX);
 /* Good sample survives poor exchanges, then ages past scheduling gate. */
 assert(network_clock_error(100,9000100,12000)>20000);
 puts("PASS clock sample selection, drift aging, expiry and overflow");
}
