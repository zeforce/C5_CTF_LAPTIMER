#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/node_poll_guard.h"
int main(void){
 node_poll_guard_t a={17,42,1,true,false,false},b=a;
 assert(node_poll_unchanged(a,b));
 b.mutation++;assert(!node_poll_unchanged(a,b));
 b=a;b.mutation+=2;assert(!node_poll_unchanged(a,b));
 b=a;b.session++;assert(!node_poll_unchanged(a,b));
 b=a;b.state=2;assert(!node_poll_unchanged(a,b));
 b=a;b.capture=false;assert(!node_poll_unchanged(a,b));
 b=a;b.stop=true;assert(!node_poll_unchanged(a,b));
 b=a;b.exclusive=true;assert(!node_poll_unchanged(a,b));
 a.exclusive=true;assert(!node_poll_unchanged(a,a));
 a.exclusive=false;a.mutation=UINT64_MAX;b=a;b.mutation=0;assert(!node_poll_unchanged(a,b));
 assert(node_prepare_unchanged(42,1,9,42,1,9));
 assert(!node_prepare_unchanged(42,1,9,43,1,9));
 assert(!node_prepare_unchanged(42,1,9,42,2,9));
 assert(!node_prepare_unchanged(42,1,9,42,1,10));
 puts("PASS health interleaving: unchanged, mutate/restore, session/state/capture/stop/exclusive changes and revision wrap");
}
