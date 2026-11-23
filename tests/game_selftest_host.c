#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/c3_controller/main/game_selftest.h"
int main(void){
 game_t *scratch=malloc(sizeof(*scratch));assert(scratch);
 assert(game_rule_selftest(scratch)==0);
 /* Repeated invocation must not depend on previous scratch contents. */
 assert(game_rule_selftest(scratch)==0);
 free(scratch);puts("PASS board boot self-test: six modes, pause/resume, terminal scoring, missing observations and SOLO failure");
}
