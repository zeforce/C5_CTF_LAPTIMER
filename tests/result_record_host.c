#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/c3_controller/main/result_record.h"
int main(void){
 result_record_t r={.revision=1,.network=513};
 for(unsigned i=0;i<9;i++){r.pages[i][0]=4;r.pages[i][1]=i;r.pages[i][2]=1;r.pages[i][4]=42;r.pages[i][8]=3;r.pages[i][10]=1;r.pages[i][12]=2;}
 r.pilot_identity[0][0]=1;r.pilot_identity[0][1]=36;r.pilot_identity[0][2]=1;memcpy(r.pilot_identity[0]+7,"PilotA",7);r.node_mac[8][5]=9;
 r.pages[1][14]=2;r.pages[1][24]=7;r.pages[1][28]=0;
 result_record_seal(&r);assert(result_record_valid(&r));assert(r.pages[1][14]==2 && r.pages[1][24]==7);
 uint8_t identity_page[32];assert(result_record_identity_page(&r,0,identity_page));assert(identity_page[0]==15 && identity_page[1]==0 && !memcmp(identity_page+15,"PilotA",7));
 assert(result_record_identity_page(&r,10,identity_page));assert(identity_page[2]==8 && identity_page[3]==1 && identity_page[13]==9);
 assert(!result_record_identity_page(&r,11,identity_page));
 r.pilot_identity[0][0]=2;result_record_seal(&r);assert(!result_record_valid(&r));r.pilot_identity[0][0]=1;result_record_seal(&r);
 r.outcome_mask=1;r.node_outcome[0][0]=3;r.node_outcome[0][1]=1;r.node_outcome[0][6]=1;
 result_record_seal(&r);assert(result_record_valid(&r));
 uint8_t outcome[32];assert(result_record_outcome_page(&r,0,outcome) && outcome[0]==16 && outcome[2]==1 && outcome[8]==3 && outcome[14]==1);
 assert(result_record_outcome_page(&r,1,outcome) && !outcome[2]);assert(!result_record_outcome_page(&r,9,outcome));
 r.node_outcome[0][5]=1;result_record_seal(&r);assert(!result_record_valid(&r));r.node_outcome[0][5]=0;result_record_seal(&r);
 /* SOLO failure survives persistence; inconsistent failure and masks reject. */
 result_record_t solo=r;for(unsigned i=0;i<9;i++)solo.pages[i][2]=3;
 solo.node_outcome[0][1]=3;solo.node_outcome[0][4]=1;solo.node_outcome[0][5]=1;solo.node_outcome[0][6]=3;
 result_record_seal(&solo);assert(result_record_valid(&solo));
 solo.node_outcome[0][2]=1;result_record_seal(&solo);assert(!result_record_valid(&solo));
 solo.node_outcome[0][2]=0;solo.outcome_mask=4;result_record_seal(&solo);assert(!result_record_valid(&solo));
 result_record_t missing=r;missing.pages[0][10]=3;missing.pages[0][12]=0;
 for(unsigned i=1;i<9;i++){missing.pages[i][10]=3;missing.pages[i][12]=0;}
 result_record_seal(&missing);assert(result_record_valid(&missing) && result_record_missing(&missing)==2);
 assert(result_record_missing(&r)==2);
 r.pages[0][10]=3;r.pages[0][12]=0;result_record_seal(&r);
 assert(result_record_valid(&r) && result_record_missing(&r)==2);
 r.pages[1][8]=1;r.pages[1][10]=1;r.pages[1][12]=0;result_record_seal(&r);
 assert(!result_record_valid(&r));
 r.pages[1][8]=3;r.pages[1][12]=2;r.pages[0][10]=1;r.pages[0][12]=2;result_record_seal(&r);
 r.pages[1][16]^=1;assert(!result_record_valid(&r));r.pages[1][16]^=1;
 r.pages[1][4]=43;result_record_seal(&r);assert(!result_record_valid(&r));r.pages[1][4]=42;
 r.pages[2][12]=0;result_record_seal(&r);assert(!result_record_valid(&r));r.pages[2][12]=2;
 r.revision=0;result_record_seal(&r);assert(!result_record_valid(&r));
 result_history_t h={.version=1};
 for(unsigned session=1;session<=6;session++){
  r.revision=session;for(unsigned i=0;i<9;i++)r.pages[i][4]=session;
  result_record_seal(&r);assert(result_history_push(&h,&r));assert(result_history_valid(&h));
 }
 assert(h.records[0].outcome_mask==1 && h.records[0].node_outcome[0][6]==1);
 assert(!memcmp(h.records[0].pilot_identity[0]+7,"PilotA",7) && h.records[0].node_mac[8][5]==9);
 assert(h.records[0].pages[1][24]==7 && h.records[0].pages[1][28]==0 && h.records[0].pages[1][14]==2);
 assert(h.count==4 && h.records[0].pages[0][4]==6 && h.records[3].pages[0][4]==3);
 r.revision=7;result_record_seal(&r);assert(result_history_push(&h,&r));assert(h.count==4 && h.records[0].revision==7);
 r=h.records[2];r.revision=8;result_record_seal(&r);assert(result_history_push(&h,&r));
 assert(h.count==4 && h.records[0].pages[0][4]==4 && h.records[1].pages[0][4]==6 && h.records[3].pages[0][4]==3);
 h.records[2].pages[0][16]^=1;assert(!result_history_valid(&h));
 puts("PASS result integrity, session consistency, missing masks and revision validation");
}
