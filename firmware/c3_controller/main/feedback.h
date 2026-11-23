#pragma once
#include <stdint.h>
#include <stdbool.h>
enum { FEEDBACK_CLICK=1, FEEDBACK_HOLD=2 };
typedef struct {
 uint8_t state, owner, fault; bool rf_online;
 uint32_t laps;
 uint8_t pilot_owner,winner;
 bool owner_rgb_valid,winner_rgb_valid;
 uint32_t owner_rgb,winner_rgb;
 uint8_t countdown_phase;
 uint32_t countdown_ms, capture_ms, capture_progress_ms;
} feedback_input_t;
typedef struct {
 feedback_input_t input;
 bool initialized, raw_button, stable_button, hold_sent;
 uint64_t raw_since, pressed_since, beep_until;
 uint64_t pattern_since;
 uint8_t pattern;
 uint32_t countdown_second;
 uint32_t transitions, clicks, holds;
} feedback_t;
typedef struct { uint32_t rgb; bool led, buzzer; uint8_t button_event; } feedback_output_t;
void feedback_init(feedback_t *f);
feedback_output_t feedback_tick(feedback_t *f,const feedback_input_t *input,bool pressed,uint64_t now_ms);
