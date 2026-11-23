#pragma once
#include <stdint.h>
#include <stdbool.h>
#define RESULT_PAGES 9
typedef struct {
 uint32_t version, network, revision;
 uint8_t pages[RESULT_PAGES][32];
 uint8_t pilot_identity[8][24]; /* enabled/channel/team/slot/colorRGB/name17 */
 uint8_t node_mac[9][6];
 uint16_t outcome_mask;
 uint8_t node_outcome[9][24];
 uint32_t checksum;
} result_record_t;
void result_record_seal(result_record_t *record);
bool result_record_valid(const result_record_t *record);
/* Union of missing nodes across team and every individual result page. */
uint16_t result_record_missing(const result_record_t *record);

#define RESULT_HISTORY_MAX 4
typedef struct {uint32_t version,count;result_record_t records[RESULT_HISTORY_MAX];} result_history_t;
bool result_history_valid(const result_history_t *history);
bool result_history_push(result_history_t *history,const result_record_t *record);

/* Identity pages: 0..7 pilots, 8..10 node MAC groups. */
bool result_record_identity_page(const result_record_t *record,unsigned part,uint8_t out[32]);

bool result_record_outcome_page(const result_record_t *record,unsigned node,uint8_t out[32]);
