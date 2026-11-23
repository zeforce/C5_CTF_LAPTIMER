#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/feedback.h"
#include "../firmware/c3_controller/main/finder.h"
int main(void){
 finder_t search={0};finder_update(&search,true,16,1000000);assert(search.power==16 && search.level==5 && search.trend==0);
 finder_update(&search,true,200,1000000);assert(search.power==16);
 finder_update(&search,true,200,2000000);assert(search.trend==1);
 for(unsigned i=3;i<12;i++)finder_update(&search,true,0,i*1000000ULL);
 assert(search.trend==2 || search.trend==0);
 finder_update(&search,false,65535,12000000);assert(!search.valid && !finder_pulse(&search,0));
 finder_update(&search,true,0,13000000);assert(!search.level && !finder_pulse(&search,0));
 finder_update(&search,false,0,0);finder_update(&search,true,65535,14000000);assert(search.level==10);
 assert(finder_pulse(&search,0) && !finder_pulse(&search,60) && finder_pulse(&search,200));
 feedback_t f;feedback_init(&f);feedback_input_t in={.rf_online=true};feedback_output_t o;
 o=feedback_tick(&f,&in,false,0);assert(o.led && !o.buzzer && !f.transitions);
 in.state=1;in.owner=1;o=feedback_tick(&f,&in,false,100);assert(o.rgb==0xff0000 && o.buzzer && f.transitions==1);
 o=feedback_tick(&f,&in,false,200);assert(!o.buzzer && f.transitions==1);
 in.owner=2;o=feedback_tick(&f,&in,false,210);assert(o.rgb==0x0000ff && f.transitions==2);
 in.fault=3;o=feedback_tick(&f,&in,false,300);assert(o.rgb==0xff8000 && !o.led && o.buzzer);
 feedback_tick(&f,&in,true,400);feedback_tick(&f,&in,false,410);feedback_tick(&f,&in,true,420);
 assert(!feedback_tick(&f,&in,true,449).button_event);
 assert(!feedback_tick(&f,&in,true,450).button_event);
 feedback_tick(&f,&in,false,500);o=feedback_tick(&f,&in,false,530);assert(o.button_event==FEEDBACK_CLICK && f.clicks==1);
 feedback_tick(&f,&in,true,600);feedback_tick(&f,&in,true,630);
 assert(!feedback_tick(&f,&in,true,2129).button_event);
 o=feedback_tick(&f,&in,true,2130);assert(o.button_event==FEEDBACK_HOLD && f.holds==1);
 assert(!feedback_tick(&f,&in,true,2200).button_event);
 feedback_tick(&f,&in,false,2300);assert(!feedback_tick(&f,&in,false,2330).button_event && f.clicks==1);
 /* Distinct capture/finish/fault patterns and no repeated tone from RGB. */
 feedback_init(&f);in=(feedback_input_t){.rf_online=true,.state=1};feedback_tick(&f,&in,false,0);
 in.owner=1;assert(feedback_tick(&f,&in,false,100).buzzer);
 assert(!feedback_tick(&f,&in,false,200).buzzer);
 assert(feedback_tick(&f,&in,false,300).buzzer);
 in.owner_rgb=123;assert(!feedback_tick(&f,&in,false,450).buzzer);
 in.capture_ms=1000;in.capture_progress_ms=1000;
 assert(feedback_tick(&f,&in,false,600).led);assert(!feedback_tick(&f,&in,false,700).led);
 in.state=3;assert(feedback_tick(&f,&in,false,800).buzzer);
 assert(!feedback_tick(&f,&in,false,950).buzzer);assert(feedback_tick(&f,&in,false,1250).buzzer);
 assert(!feedback_tick(&f,&in,false,1500).buzzer);
 feedback_init(&f);in=(feedback_input_t){.rf_online=true};feedback_tick(&f,&in,false,0);
 in.countdown_phase=1;in.countdown_ms=3000;assert(!feedback_tick(&f,&in,false,20).buzzer);
 in.countdown_phase=2;assert(feedback_tick(&f,&in,false,40).buzzer);
 in.countdown_ms=2900;assert(!feedback_tick(&f,&in,false,140).buzzer);
 in.countdown_ms=2000;assert(feedback_tick(&f,&in,false,1040).buzzer);
 in.countdown_phase=0;in.countdown_ms=0;assert(!feedback_tick(&f,&in,false,1200).buzzer);
 in.fault=3;assert(feedback_tick(&f,&in,false,1300).buzzer);
 assert(!feedback_tick(&f,&in,false,1450).buzzer);assert(feedback_tick(&f,&in,false,1500).buzzer);
 assert(!feedback_tick(&f,&in,false,1900).buzzer);
 puts("PASS state feedback, bounded buzzer, button bounce, one click, one hold, no release click after hold");
}
