#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/node_spi_budget.h"
int main(void){
 for(unsigned command=0;command<256;command++){
  int64_t expected=command==9?2000000:100000;
  assert(node_spi_budget_us(command)==expected);
  assert(node_spi_default_deadline(command,123)==123+expected);
  assert(node_spi_default_deadline(command,INT64_MAX-5)==INT64_MAX);
  assert(node_spi_mutates(command)==(command==4 || command==8 || command==9 || command==14 || command==15 || command==17 || command==20));
 }
 puts("PASS ordinary READY budget, legacy scan exception and deadline saturation");
}
