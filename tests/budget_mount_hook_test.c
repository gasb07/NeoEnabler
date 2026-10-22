/* Offline production-header hook lifecycle tests. No console or network. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1
#define KERN_PROC_PROC 8
struct kinfo_proc {int ki_structsize;pid_t ki_pid;char ki_stat;char ki_comm[20];};
typedef struct {uint64_t mapbase,mapsize,textsize,eh_frame_hdr;} dynlib_obj_t;
#define MAIN_BASE 0x10000000ULL
#define LIB_BASE 0x20000000ULL
static unsigned char main_memory[0x2100000],lib_memory[0x50000];
static int state,permission,stop_count,continue_count,continue_fail,stop_timeout;
static int writes,readback_fail,pending_readback,cave_write_fail,change_on_stop;
static uint64_t call_address,cave_address;
static unsigned char *at(uint64_t a,size_t n){
 if(a>=MAIN_BASE&&a-MAIN_BASE<=sizeof main_memory&&n<=sizeof main_memory-(a-MAIN_BASE))return main_memory+(a-MAIN_BASE);
 if(a>=LIB_BASE&&a-LIB_BASE<=sizeof lib_memory&&n<=sizeof lib_memory-(a-LIB_BASE))return lib_memory+(a-LIB_BASE);
 assert(!"mock address outside mapped images");return NULL;
}
static int find_proc(const char *s){assert(!strcmp(s,"SceShellCore"));return 42;}
static int kernel_dynlib_obj(pid_t p,uint32_t h,dynlib_obj_t *o){assert(p==42);o->mapbase=h?LIB_BASE:MAIN_BASE;o->mapsize=h?sizeof lib_memory:sizeof main_memory;return 0;}
static int kernel_dynlib_handle(pid_t p,const char*s,uint32_t*h){assert(p==42&&!strcmp(s,"libkernel_sys.sprx"));*h=1;return 0;}
static int kernel_proc_copyout(pid_t p,uint64_t a,void*b,size_t n){assert(p==42);if(pending_readback&&a==call_address&&n==5){pending_readback=0;return -1;}memcpy(b,at(a,n),n);return 0;}
static int kernel_proc_copyin(pid_t p,const void*b,uint64_t a,size_t n){
 assert(p==42);if(n==5||n==1024)assert(state==4);writes++;
 if(cave_write_fail&&a==cave_address&&n==1024){cave_write_fail=0;memcpy(at(a,n),b,n/2);return -1;}
 memcpy(at(a,n),b,n);
 if(readback_fail&&a==call_address&&n==5){readback_fail=0;pending_readback=1;}
 return 0;
}
static int kernel_get_vmem_protection(pid_t p,uint64_t a,size_t n){assert(p==42&&!(a&0x3fff)&&n==0x4000);return permission;}
static int mock_sysctl(const int*m,unsigned n,void*b,size_t*z,const void*v,size_t l){
 assert(n==4&&m[2]==KERN_PROC_PROC&&m[3]==0&&!v&&!l);
 if(!b){*z=sizeof(struct kinfo_proc);return 0;}
 assert(*z>=sizeof(struct kinfo_proc));
 struct kinfo_proc k={.ki_structsize=sizeof k,.ki_pid=42,.ki_stat=(char)state};
 strcpy(k.ki_comm,"SceShellCore");memcpy(b,&k,sizeof k);*z=sizeof k;return 0;
}
static int mock_kill(pid_t p,int sig){assert(p==42);if(sig==SIGSTOP){stop_count++;if(!stop_timeout)state=4;if(change_on_stop){uint32_t x=1;memcpy(at(cave_address+change_on_stop,4),&x,4);}return 0;}assert(sig==SIGCONT);continue_count++;if(continue_fail)return -1;state=2;return 0;}
static int mock_usleep(unsigned u){(void)u;return 0;}
#define sysctl mock_sysctl
#define kill mock_kill
#define usleep mock_usleep
#include "../src/budget_mount_hook.h"
static void reset(uint32_t fw){
 memset(&bm,0,sizeof bm);bm_reuse_allowed=0;memset(main_memory,0,sizeof main_memory);memset(lib_memory,0,sizeof lib_memory);
 state=2;permission=7;stop_count=continue_count=continue_fail=stop_timeout=writes=readback_fail=pending_readback=cave_write_fail=change_on_stop=0;
 const bm_profile*p=bm_find(fw);assert(p);call_address=MAIN_BASE+p->call;cave_address=MAIN_BASE+p->cave;
 const unsigned char pro[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xe8,0x18,0,0};
 unsigned char wrapper[sizeof pro];memcpy(wrapper,pro,sizeof pro);wrapper[16]=0x98;wrapper[17]=1;
 memcpy(at(MAIN_BASE+p->function,sizeof pro),pro,sizeof pro);memcpy(at(call_address,5),p->call_bytes,5);memcpy(at(MAIN_BASE+p->import_,6),p->stub,6);
 uint64_t target=LIB_BASE+p->wrapper;memcpy(at(MAIN_BASE+p->got,8),&target,8);memcpy(at(target,sizeof wrapper),wrapper,sizeof wrapper);
}
static void field(unsigned off,uint32_t value){assert(!bm_field(off,value));}
int main(void){
 for(unsigned i=0;i<sizeof bm_profiles/sizeof bm_profiles[0];i++){
  uint32_t fw=bm_profiles[i].fw;const bm_profile*p=&bm_profiles[i];
  reset(fw);assert(!bm_install(fw,123456));assert(bm.attached&&!bm.fatal&&state==2&&stop_count==1&&continue_count==1);assert(!memcmp(at(call_address,5),bm.redirect,5));
  bm_mailbox mail;assert(!bm_mail(&mail)&&!mail.enabled&&!mail.active);field(MS_ENABLED,1);assert(!bm_command(1,9));assert(!bm_mail(&mail)&&mail.response_seq==9&&mail.command==1);
  field(MS_GATE,1);assert(bm_remove()==-1&&bm.attached);field(MS_GATE,0);field(MS_DIRTY,1);assert(bm_remove()==-1&&bm.attached);field(MS_DIRTY,0);
  assert(!bm_remove()&&!bm.attached&&state==2);assert(!memcmp(at(call_address,5),p->call_bytes,5));
  assert(!bm_install(fw,123456)&&bm.cave==cave_address+1024);assert(!bm_remove()); /* never reuse retained code */
  reset(fw);at(call_address,5)[0]^=1;assert(bm_install(fw,1)==-4&&!writes&&!stop_count);
  assert(strstr(bm.failure,"BeginAppMount2 call")&&bm.check_address==call_address);
  reset(fw);at(MAIN_BASE+p->got,8)[0]^=1;assert(bm_install(fw,1)==-4&&!writes);
  assert(strstr(bm.failure,"GOT target differs")&&bm.check_address==MAIN_BASE+p->got);
  reset(fw);at(MAIN_BASE+p->function,20)[0]^=1;assert(bm_install(fw,1)==-4&&!writes);
  reset(fw);at(LIB_BASE+p->wrapper,20)[0]^=1;assert(bm_install(fw,1)==-4&&!writes);
  reset(fw);permission=5;assert(bm_install(fw,1)==-4&&!writes);
  assert(strstr(bm.failure,"protection 5")&&bm.check_address==(call_address&~0x3fffULL));
  /* Other code before our reserved pool must not block installation or be
   * overwritten. Reproduce the observed 13.60 prefix, and a dense prefix. */
  reset(fw);size_t prefix_size=p->cave-p->padding;
  unsigned char prefix[4096];assert(prefix_size<=sizeof prefix);
  memset(prefix,0xcc,prefix_size);memcpy(at(MAIN_BASE+p->padding,prefix_size),prefix,prefix_size);
  assert(!bm_install(fw,1)&&bm.attached);
  assert(!memcmp(at(MAIN_BASE+p->padding,prefix_size),prefix,prefix_size));
  assert(!bm_remove());assert(!memcmp(at(MAIN_BASE+p->padding,prefix_size),prefix,prefix_size));
  if(fw==0x13600000){
   reset(fw);
   const unsigned char observed[]={0x66,0x90,0x53,0x41,0x54,0x49,0x89,0xf4,0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x48,0x83,0xec,8,0xbf,1,0,0,0,0xe8,0x82,0,0,0,0x48,0x83};
   memcpy(at(MAIN_BASE+0x1962700,sizeof observed),observed,sizeof observed);
   assert(!bm_install(fw,1)&&bm.attached);
   assert(!memcmp(at(MAIN_BASE+0x1962700,sizeof observed),observed,sizeof observed));
   assert(!bm_remove());
  }
  /* Any unknown nonzero data INSIDE the pool must still fail before writes. */
  for(uint32_t slot=p->cave;slot<p->end;slot+=1024){
   reset(fw);at(MAIN_BASE+slot,1024)[1023]=1;
   assert(bm_install(fw,1)==-4&&!writes&&!stop_count);
   assert(strstr(bm.failure,"Reservation occupied")&&bm.check_address==MAIN_BASE+slot);
  }
  /* Killing the payload without Quit (rest mode): an idle armed orphan of ours is disarmed, the call restored and a
     fresh slot used; the orphan stays as a retained slot. */
  reset(fw);assert(!bm_install(fw,1));field(MS_ENABLED,1);
  memset(&bm,0,sizeof bm);writes=stop_count=continue_count=0;
  assert(!bm_install(fw,1)&&bm.recovered&&bm.attached&&state==2&&stop_count==2&&continue_count==2);
  assert(bm.cave==cave_address+1024&&!memcmp(at(call_address,5),bm.redirect,5));
  {uint32_t v;memcpy(&v,at(cave_address+MS_ENABLED,4),4);assert(!v);}
  assert(!bm_remove()&&!memcmp(at(call_address,5),p->call_bytes,5));
  /* Previous run confirmed gone: the idle orphan's slot is reused in place (same redirect, same code, cleared mailbox,
     new cycles) instead of spending a fresh slot. */
  reset(fw);assert(!bm_install(fw,1));field(MS_ENABLED,1);field(MS_SEQ,7);
  {unsigned char redirect[5];memcpy(redirect,bm.redirect,5);uint64_t old_cave=bm.cave;
   memset(&bm,0,sizeof bm);writes=stop_count=continue_count=0;bm_reuse_allowed=1;
   assert(!bm_install(fw,999)&&bm.attached&&bm.recovered==2&&bm.cave==old_cave&&state==2&&stop_count==1&&continue_count==1);
   assert(!memcmp(at(call_address,5),redirect,5)&&!memcmp(bm.redirect,redirect,5));
   uint32_t v;uint64_t c;memcpy(&v,at(old_cave+MS_ENABLED,4),4);assert(!v);memcpy(&v,at(old_cave+MS_SEQ,4),4);assert(!v);
   memcpy(&c,at(old_cave+MS_WAIT_CYCLES,8),8);assert(c==999);assert(!memcmp(at(old_cave,MS_ORIGINAL),step_code,MS_ORIGINAL));
   for(size_t z=1024;z<2048;z++)assert(!at(cave_address,2048)[z]);           /* no fresh slot used */
   assert(!bm_remove()&&!memcmp(at(call_address,5),p->call_bytes,5));}
  /* Reuse allowed but the orphan is busy: still refused without writes to the call. */
  reset(fw);assert(!bm_install(fw,1));field(MS_ENABLED,1);field(MS_GATE,1);
  {unsigned char redirect[5];memcpy(redirect,bm.redirect,5);
   memset(&bm,0,sizeof bm);writes=stop_count=continue_count=0;bm_reuse_allowed=1;
   assert(bm_install(fw,1)==-4&&!writes&&!bm.attached&&strstr(bm.failure,"busy")&&!memcmp(at(call_address,5),redirect,5));}
  /* A busy orphan (in a call, holding the gate or with DATA dirty) is never touched. */
  {const unsigned busy[]={MS_ACTIVE,MS_GATE,MS_DIRTY};
   for(unsigned i=0;i<3;i++){
    reset(fw);assert(!bm_install(fw,1));field(MS_ENABLED,1);field(busy[i],1);
    unsigned char redirect[5];memcpy(redirect,bm.redirect,5);
    memset(&bm,0,sizeof bm);writes=stop_count=continue_count=0;
    assert(bm_install(fw,1)==-4&&!writes&&stop_count==1&&continue_count==1&&state==2&&!bm.attached);
    assert(strstr(bm.failure,"busy")&&!memcmp(at(call_address,5),redirect,5));
   }}
  /* A redirect that is not ours (outside the reservation or other code) is still refused without any write. */
  reset(fw);at(call_address,5)[1]^=0x40;assert(bm_install(fw,1)==-4&&!writes&&!stop_count&&strstr(bm.failure,"BeginAppMount2 call"));
  reset(fw);assert(!bm_install(fw,1));at(cave_address,1)[0]^=1;memset(&bm,0,sizeof bm);writes=stop_count=0;
  assert(bm_install(fw,1)==-4&&!writes&&!stop_count&&strstr(bm.failure,"BeginAppMount2 call"));
  reset(fw);readback_fail=1;assert(bm_install(fw,1)==-9&&!bm.attached&&state==2);assert(!memcmp(at(call_address,5),p->call_bytes,5));
  reset(fw);cave_write_fail=1;assert(bm_install(fw,1)==-8&&!bm.attached&&!bm.fatal&&state==2);for(size_t z=0;z<1024;z++)assert(!at(cave_address,1024)[z]);assert(!bm_install(fw,1));assert(!bm_remove());
  reset(fw);continue_fail=1;assert(bm_install(fw,1)==-101&&bm.fatal&&bm.stopped&&state==4);
  reset(fw);state=4;assert(bm_install(fw,1)==-6&&!writes&&!continue_count); /* do not resume another owner's stop */
  reset(fw);stop_timeout=1;assert(bm_install(fw,1)==-6&&!writes&&continue_count==1);
  reset(fw);assert(!bm_install(fw,1));field(MS_ENABLED,1);change_on_stop=MS_ACTIVE;assert(bm_remove()==-1&&bm.attached&&state==2);assert(!memcmp(at(call_address,5),bm.redirect,5));
  reset(fw);assert(!bm_install(fw,1));change_on_stop=MS_GATE;assert(bm_remove()==-1&&bm.attached&&state==2);
  reset(fw);assert(!bm_install(fw,1));change_on_stop=MS_DIRTY;assert(bm_remove()==-1&&bm.attached&&state==2);
  reset(fw);assert(!bm_install(fw,1));continue_fail=1;assert(bm_remove()==-101&&bm.fatal&&bm.stopped);
 }
 assert(bm_find(0x14000000)==NULL);
 puts("PASS: production mount hook profiles, guards, stopped writes, rollback, retained slots, orphan recovery and reuse, detach races and resume failures");return 0;
}
