#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Compared under the controller lock before and after yielding health IO. */
typedef struct {
 uint64_t mutation;
 uint32_t session;
 uint8_t state;
 bool capture,stop,exclusive;
} node_poll_guard_t;
static inline bool node_poll_unchanged(node_poll_guard_t a,node_poll_guard_t b){
 return !a.exclusive && !b.exclusive && a.mutation==b.mutation &&
  a.session==b.session && a.state==b.state && a.capture==b.capture && a.stop==b.stop;
}

/* Preparation may yield only while the same match and RF mutation revision
 * remain current. The controller rejects start/resume while preparation owns
 * RF, so a cancellation cannot return to its former state during the yield. */
static inline bool node_prepare_unchanged(uint32_t session,uint8_t state,uint64_t revision,
 uint32_t current_session,uint8_t current_state,uint64_t current_revision){
 return session==current_session && state==current_state && revision==current_revision;
}
