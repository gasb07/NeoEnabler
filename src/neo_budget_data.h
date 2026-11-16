/* GPL-3.0-only. NEO DMEM budget extension.
 * Fixed firmware DATA layouts. Requires a confirmed, serialized launch hold.
 * This backend does not install a launch hook or establish that hold.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NEO_DATA_BUDGET_TEST
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ps5/kernel.h>
#define MBD_DATA() ((uint64_t)KERNEL_ADDRESS_DATA_BASE)
#define MBD_KREAD(a,p,n) kernel_copyout(a,p,n)
#define MBD_KWRITE(p,a,n) kernel_copyin(p,a,n)
#endif

enum {MBD_UNKNOWN=-1, MBD_STOCK=0, MBD_8GIB=1, MBD_STEP=2};

#define MBD_DELTA        0xC0000000ULL
#define MBD_EXPECTED_STOCK 0x18c000000ULL

typedef struct {
 uint32_t fw;
 uint32_t ext_global;     /* 8-byte extension global; byte+3 is the write target */
 uint32_t container_ptr;  /* pointer to active budget container */
 uint32_t backing_base;
 uint32_t references;
 uint32_t profile;
 uint32_t maximum;
} mbd_layout;

static const mbd_layout mbd_layouts[]={
 {0x10010000, 0x2d5c0b8, 0x2d55fe0, 0x2d55fb8, 0x2d5c110, 0x2d5d400, 0x2d5d418},
 /* 12.20 shares the 12.70 offsets, verified during launch and teardown. */
 {0x12200000, 0x2e880b8, 0x2e81fe0, 0x2e81fb8, 0x2e88110, 0x2e89400, 0x2e89418},
 {0x12700000, 0x2e880b8, 0x2e81fe0, 0x2e81fb8, 0x2e88110, 0x2e89400, 0x2e89418},
 {0x13200000, 0x2ee00b8, 0x2ed9fe0, 0x2ed9fb8, 0x2ee0110, 0x2ee1400, 0x2ee1418},
 {0x13600000, 0x2ef40b8, 0x2eedfe0, 0x2eedfb8, 0x2ef4110, 0x2ef5400, 0x2ef5418},
};
#define MBD_LAYOUT_COUNT (sizeof mbd_layouts/sizeof mbd_layouts[0])

static inline const mbd_layout*mbd_find_layout(uint32_t fw){
 for(unsigned i=0;i<MBD_LAYOUT_COUNT;i++)
  if(mbd_layouts[i].fw==fw)return&mbd_layouts[i];
 return NULL;
}

typedef struct {
 uint64_t container, extension, maximum, field38, reserved, available, backing_base;
 uint32_t app_id, category, references, profile, flags;
 unsigned mode;
} mbd_snapshot;

static inline int mbd_heap_pointer(uint64_t p){
 return p>=0xffff800000000000ULL&&p<0xffffffff00000000ULL&&!(p&7);
}

static inline int mbd_read_snapshot(const mbd_layout *L, mbd_snapshot *out){
 uint64_t dbase=MBD_DATA();
 memset(out,0,sizeof(*out));
 if(MBD_KREAD(dbase+L->ext_global,&out->extension,8)||
    MBD_KREAD(dbase+L->maximum,&out->maximum,8)||
    MBD_KREAD(dbase+L->references,&out->references,4)||
    MBD_KREAD(dbase+L->profile,&out->profile,4)||
    MBD_KREAD(dbase+L->container_ptr,&out->container,8))return -1;
 if(!out->container)return 0;
 if(!mbd_heap_pointer(out->container))return -1;
 unsigned char bytes[0x148];
 if(MBD_KREAD(out->container,bytes,sizeof(bytes)))return -1;
 uint64_t descriptor,after;
 memcpy(&descriptor,bytes+8,8);
 memcpy(&out->field38,bytes+0x38,8);
 memcpy(&out->reserved,bytes+0x40,8);
 memcpy(&out->available,bytes+0x80,8);
 memcpy(&out->category,bytes+0x118,4);
 memcpy(&out->app_id,bytes+0x11c,4);
 memcpy(&out->flags,bytes+0x140,4);
 if(MBD_KREAD(dbase+L->backing_base,&out->backing_base,8))return -1;
 if(!mbd_heap_pointer(descriptor)||
    MBD_KREAD(descriptor+0x58,&out->mode,1)||
    MBD_KREAD(dbase+L->container_ptr,&after,8)||after!=out->container)return -1;
 return 0;
}

static inline int mbd_eligible(const mbd_snapshot *s){
 return s->container>=0xffff800000000000ULL&&s->container<0xffffffff00000000ULL&&
        !(s->container&7)&&s->app_id&&s->category==0xf000&&
        s->mode==2&&!(s->flags&0x40)&&!s->field38&&!s->reserved&&
        !s->extension&&!s->maximum&&s->references==1&&s->profile==1&&
        s->backing_base==0x100000000ULL&&s->available>=s->backing_base&&
        s->available-s->backing_base>=MBD_EXPECTED_STOCK+MBD_DELTA;
}

typedef struct{int touched,owned,restored,failed;uint64_t previous;} mbd_transaction;

enum{MBD_TX_OK=0,MBD_TX_REFUSED=1,MBD_TX_WRITE_FAILED=2,MBD_TX_RECOVERY=3};

static inline int mbd_tx_read_global(const mbd_layout *L,uint64_t *out){
 return MBD_KREAD(MBD_DATA()+L->ext_global,out,8);
}

static inline int mbd_tx_write_byte(const mbd_layout *L,unsigned char value){
 return MBD_KWRITE(&value,MBD_DATA()+L->ext_global+3,1);
}

static inline int mbd_restore(const mbd_layout *L,mbd_transaction *tx){
 if(!L||!tx)return MBD_TX_RECOVERY;
 if(!tx->touched){tx->restored=1;return MBD_TX_OK;}
 uint64_t current=0;
 if(mbd_tx_read_global(L,&current))return MBD_TX_RECOVERY;
 if(current==tx->previous){tx->owned=0;tx->restored=1;return MBD_TX_OK;}
 if(current!=tx->previous+MBD_DELTA)return MBD_TX_RECOVERY;
 (void)mbd_tx_write_byte(L,0);
 if(mbd_tx_read_global(L,&current)||current!=tx->previous)return MBD_TX_RECOVERY;
 tx->owned=0;tx->restored=1;
 return MBD_TX_OK;
}

static inline int mbd_apply(const mbd_layout *L,mbd_transaction *tx,const mbd_snapshot *s){
 if(!tx)return MBD_TX_REFUSED;
 /* Never discard an outstanding rollback obligation. */
 if(tx->touched&&!tx->restored)return MBD_TX_RECOVERY;
 memset(tx,0,sizeof(*tx));
 if(!L||!s||!mbd_eligible(s))return MBD_TX_REFUSED;
 mbd_snapshot current;
 if(mbd_read_snapshot(L,&current)||memcmp(&current,s,sizeof current)||!mbd_eligible(&current))
  return MBD_TX_REFUSED;
 uint64_t before=0,after=0;
 if(mbd_tx_read_global(L,&before)||before)return MBD_TX_REFUSED;
 tx->previous=before;tx->touched=1;
 int rc=mbd_tx_write_byte(L,(unsigned char)(MBD_DELTA>>24));
 int rr=mbd_tx_read_global(L,&after);
 if(!rc&&!rr&&after==MBD_DELTA){tx->owned=1;return MBD_TX_OK;}
 tx->failed=1;
 return mbd_restore(L,tx)==MBD_TX_OK?MBD_TX_WRITE_FAILED:MBD_TX_RECOVERY;
}

static inline int mbd_state(const mbd_layout *L){
 uint64_t ext=0;
 if(mbd_tx_read_global(L,&ext))return MBD_UNKNOWN;
 if(ext==MBD_DELTA)return MBD_8GIB;
 if(ext==0)return MBD_STOCK;
 return MBD_UNKNOWN;
}

