/* GPL-3.0-only. Host replay of the DATA transaction; never kernel TEXT. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define NEO_DATA_BUDGET_TEST
#define MBD_DATA() 0xffffffffa0000000ULL
static int read_data(uint64_t,void*,size_t);
static int write_data(const void*,uint64_t,size_t);
#define MBD_KREAD(a,p,n) read_data(a,p,n)
#define MBD_KWRITE(p,a,n) write_data(p,a,n)
#include "../src/neo_budget_data.h"
static const mbd_layout *layout;
static mbd_snapshot live;
static uint64_t descriptor=0xffff800000002000ULL;
static int writes,fail_write,fail_restore,foreign_write,fail_read;
static int read_data(uint64_t a,void *p,size_t n){
 if(fail_read)return -1;
 /* Any unexpected address, including TEXT, is a test failure. */
 #define FIELD(off,field) if(a==MBD_DATA()+layout->off){assert(n==sizeof live.field);memcpy(p,&live.field,n);return 0;}
 FIELD(ext_global,extension)
 FIELD(maximum,maximum)
 FIELD(container_ptr,container)
 FIELD(backing_base,backing_base)
 FIELD(references,references)
 FIELD(profile,profile)
 #undef FIELD
 if(a==live.container){
  assert(n==0x148);memset(p,0,n);
  #define PUT(off,field) memcpy((char*)p+off,&live.field,sizeof live.field)
  memcpy((char*)p+8,&descriptor,8);
  PUT(0x38,field38);PUT(0x40,reserved);PUT(0x80,available);
  PUT(0x118,category);PUT(0x11c,app_id);PUT(0x140,flags);
  #undef PUT
  return 0;
 }
 if(a==descriptor+0x58){assert(n==1);*(unsigned char*)p=(unsigned char)live.mode;return 0;}
 assert(!"unexpected read");return -1;
}
static int write_data(const void *p,uint64_t a,size_t n){
 assert(a==MBD_DATA()+layout->ext_global+3&&n==1);writes++;
 if(fail_restore&&writes>1)return -1;
 if(foreign_write){live.extension=0x1234;return -1;}
 live.extension=(live.extension&~0xff000000ULL)|((uint64_t)*(const unsigned char*)p<<24);
 return fail_write&&writes==1?-1:0;
}
static void reset(uint32_t fw){
 layout=mbd_find_layout(fw);assert(layout);
 memset(&live,0,sizeof live);live.container=0xffff800000001000ULL;
 live.app_id=1;live.category=0xf000;live.mode=2;live.references=live.profile=1;
 live.backing_base=0x100000000ULL;
 live.available=live.backing_base+MBD_EXPECTED_STOCK+MBD_DELTA;
 writes=fail_write=fail_restore=foreign_write=fail_read=0;
}
int main(void){
 /* The 10.01 layout was measured on a Pro console: one PS4 mount dump gave exactly
  * one 4 GiB qword whose +0x28 was a live container pointer with references=1,
  * profile=1, maximum=0, extension=0, and the idle dump of the same firmware had
  * container=0 and references=0 at the same offsets. */
 assert(mbd_find_layout(0x10010000)&&
        mbd_find_layout(0x10010000)->ext_global==0x2d5c0b8&&
        mbd_find_layout(0x10010000)->container_ptr==0x2d55fe0&&
        mbd_find_layout(0x10010000)->backing_base==0x2d55fb8);
 assert(!mbd_find_layout(0x13500000));
 const uint32_t firmwares[]={0x12200000,0x13200000,0x13600000};
 const uint32_t extension_offsets[]={0x2e880b8,0x2ee00b8,0x2ef40b8};
 for(unsigned i=0;i<sizeof firmwares/sizeof firmwares[0];i++){
  reset(firmwares[i]);
  assert(layout->ext_global==extension_offsets[i]);
  mbd_snapshot s;mbd_transaction tx={0};
  assert(!mbd_read_snapshot(layout,&s)&&mbd_eligible(&s));
  assert(mbd_state(layout)==MBD_STOCK);
  assert(mbd_apply(layout,&tx,&s)==MBD_TX_OK&&writes==1&&tx.owned);
  assert(live.extension==MBD_DELTA&&mbd_state(layout)==MBD_8GIB);
  assert(mbd_apply(layout,&tx,&s)==MBD_TX_RECOVERY&&writes==1);
  assert(!mbd_restore(layout,&tx)&&tx.restored&&live.extension==0);
  assert(!mbd_restore(layout,&tx)&&writes==2);
  /* Every lifecycle guard must refuse before any write. */
  for(int guard=0;guard<14;guard++){
   reset(firmwares[i]);
   switch(guard){
    case 0:live.flags=0x40;break;case 1:live.mode=1;break;
    case 2:live.category=0;break;case 3:live.app_id=0;break;
    case 4:live.field38=1;break;case 5:live.reserved=1;break;
    case 6:live.extension=1;break;case 7:live.maximum=1;break;
    case 8:live.references=2;break;case 9:live.profile=2;break;
    case 10:live.backing_base=0;break;case 11:live.available--;break;
    case 12:live.container=0;break;case 13:live.container++;break;
   }
   memset(&tx,0,sizeof tx);(void)mbd_read_snapshot(layout,&s);
   assert(mbd_apply(layout,&tx,&s)==MBD_TX_REFUSED&&writes==0);
  }
  reset(firmwares[i]);memset(&tx,0,sizeof tx);assert(!mbd_read_snapshot(layout,&s));
  live.app_id++;assert(mbd_apply(layout,&tx,&s)==MBD_TX_REFUSED&&!writes);
  reset(firmwares[i]);assert(!mbd_read_snapshot(layout,&s));fail_write=1;
  assert(mbd_apply(layout,&tx,&s)==MBD_TX_WRITE_FAILED&&tx.restored&&!live.extension);
  reset(firmwares[i]);memset(&tx,0,sizeof tx);assert(!mbd_read_snapshot(layout,&s));
  fail_write=fail_restore=1;
  assert(mbd_apply(layout,&tx,&s)==MBD_TX_RECOVERY&&live.extension==MBD_DELTA);
  fail_restore=0;assert(!mbd_restore(layout,&tx));
  reset(firmwares[i]);memset(&tx,0,sizeof tx);assert(!mbd_read_snapshot(layout,&s));
  foreign_write=1;
  assert(mbd_apply(layout,&tx,&s)==MBD_TX_RECOVERY&&writes==1&&live.extension==0x1234);
  reset(firmwares[i]);memset(&tx,0,sizeof tx);assert(!mbd_read_snapshot(layout,&s));
  fail_read=1;assert(mbd_apply(layout,&tx,&s)==MBD_TX_REFUSED&&!writes);
 }
 puts("DATA transaction: firmware layouts, guards, races and rollback passed");
}
