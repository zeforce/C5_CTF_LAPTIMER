#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t version,network,count;uint8_t mac[8][6];uint32_t checksum;} network_roster_t;
void network_roster_seal(network_roster_t *r);
bool network_roster_valid(const network_roster_t *r,uint32_t network);

/* old=NULL appends, replacement=NULL removes; failure leaves input unchanged. */
bool network_roster_edit(network_roster_t *r,const uint8_t *old,const uint8_t *replacement);
