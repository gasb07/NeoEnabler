/* GPL-3.0-only. Firmware-specific BeginAppMount2 barrier for DATA budgets.
 * Uses only userland code and DATA. No kernel TEXT probing or modification.
 */
#pragma once
#include "step_shim_data.h"

typedef struct {
 uint32_t fw, function, call, import_, got, wrapper, padding, cave, end;
 unsigned char call_bytes[5], stub[6];
} bm_profile;
static const bm_profile bm_profiles[]={
 {0x13200000,0x6d6dc0,0x6d7c11,0x195b690,0x2067458,0x42850,0x19619ec,0x1962000,0x1964000,
  {0xe8,0x7a,0x3a,0x28,1},{0xff,0x25,0xc2,0xbd,0x70,0}},
 {0x13600000,0x6d7af0,0x6d8941,0x195c3b0,0x2067460,0x42850,0x19626fc,0x1963000,0x1964000,
  {0xe8,0x6a,0x3a,0x28,1},{0xff,0x25,0xaa,0xb0,0x70,0}},
};
typedef struct {
 uint32_t enabled,active,gate,seq,response_seq,command,phase,category;
 int32_t result;
 char title[12];
 uint32_t blocked,refused,expired,dirty;
 char magic[8];
} bm_mailbox;
_Static_assert(sizeof(bm_mailbox)==MS_MAGIC+8-MS_ENABLED,"mailbox ABI");
typedef struct {
 const bm_profile *profile;
 pid_t pid; uint64_t base,lib,cave,cycles;
 unsigned char saved[5],redirect[5];
 int attached,stopped,fatal;
 char failure[160];
 uint64_t check_address;
 uint64_t main_text_size,main_map_size,frame_header;
 char failure_preview[97],slot_summary[160];
 int recovered;
} bm_hook;
static bm_hook bm;
/* Set by the manager when the previous run is confirmed gone (killed, exited or quit): its idle slot can be reused.
 * bm_reuse_slot is that run's slot offset (0 when it did not record one). */
static int bm_reuse_allowed;
static uint32_t bm_reuse_slot;
static const bm_profile *bm_find(uint32_t fw){
 for(unsigned i=0;i<sizeof bm_profiles/sizeof bm_profiles[0];i++)if(bm_profiles[i].fw==fw)return &bm_profiles[i];
 return NULL;
}
static int bm_read(uint64_t a,void *p,size_t n){return kernel_proc_copyout(bm.pid,a,p,n);}
static int bm_store(uint64_t a,const void *p,size_t n){
 unsigned char back[1024];
 return n>sizeof back||kernel_proc_copyin(bm.pid,(void*)p,a,n)||bm_read(a,back,n)||memcmp(p,back,n)?-1:0;
}
static int bm_state(void){
 /* KERN_PROC_PID is not a reliable full kinfo_proc query on this target.
  * Use the same bounded KERN_PROC_PROC enumeration as the tested observer. */
 int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t n=0;
 if(sysctl(mib,4,NULL,&n,NULL,0)||!n||n>32*1024*1024)return -1;
 n+=16*sizeof(struct kinfo_proc);unsigned char *bytes=malloc(n);if(!bytes)return -1;
 int state=-1;
 if(!sysctl(mib,4,bytes,&n,NULL,0)){
  for(size_t off=0;off+sizeof(struct kinfo_proc)<=n;){
   struct kinfo_proc *k=(void*)(bytes+off);
   if(k->ki_structsize<(int)sizeof *k||(size_t)k->ki_structsize>n-off)break;
   if(k->ki_pid==bm.pid&&!strncmp(k->ki_comm,"SceShellCore",sizeof k->ki_comm)){
    state=(unsigned char)k->ki_stat;break;
   }
   off+=(size_t)k->ki_structsize;
  }
 }
 free(bytes);return state;
}
static int bm_stop(void){
 int state=bm_state();if(state<0||state==4||kill(bm.pid,SIGSTOP))return -1;
 for(int i=0;i<100;i++){if(bm_state()==4){bm.stopped=1;return 0;}usleep(1000);}
 if(kill(bm.pid,SIGCONT)){bm.fatal=1;bm.stopped=1;}return -1;
}
static int bm_resume(void){
 if(!bm.stopped)return 0;
 if(kill(bm.pid,SIGCONT))return -1;
 bm.stopped=0;return 0;
}
static int bm_fail(const char *why,uint64_t address){
 bm.check_address=address;snprintf(bm.failure,sizeof bm.failure,"%s",why);return -1;
}
static int bm_bytes(uint64_t address,const unsigned char *expected,size_t n,const char *name){
 unsigned char actual[24];
 if(n>sizeof actual||bm_read(address,actual,n)){
  bm.check_address=address;snprintf(bm.failure,sizeof bm.failure,"%s unreadable",name);return -1;
 }
 if(!memcmp(actual,expected,n))return 0;
 size_t i=0;while(i<n&&actual[i]==expected[i])i++;
 bm.check_address=address+i;
 snprintf(bm.failure,sizeof bm.failure,"%s differs at +0x%zx: expected %02x, read %02x",name,i,expected[i],actual[i]);
 return -1;
}
static int bm_valid(void){
 const bm_profile *p=bm.profile;uint64_t got;
 const unsigned char pro[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xe8,0x18,0,0};
 const unsigned char wrapper[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,1,0,0};
 if(bm_bytes(bm.base+p->function,pro,sizeof pro,"ShellCore prologue")||
    bm_bytes(bm.base+p->call,p->call_bytes,5,"BeginAppMount2 call")||
    bm_bytes(bm.base+p->import_,p->stub,6,"BeginAppMount2 import"))return -1;
 if(bm_read(bm.base+p->got,&got,8))return bm_fail("libkernel_sys GOT unreadable",bm.base+p->got);
 if(got!=bm.lib+p->wrapper){
  bm.check_address=bm.base+p->got;
  snprintf(bm.failure,sizeof bm.failure,"GOT target differs: expected 0x%lx, read 0x%lx",(unsigned long)(bm.lib+p->wrapper),(unsigned long)got);return -1;
 }
 return bm_bytes(got,wrapper,sizeof wrapper,"libkernel_sys wrapper");
}
/* Recognize our own disarmed retained shim; never reuse its reservation. */
static int bm_retained(const unsigned char *b){
 if(memcmp(b,step_code,MS_ORIGINAL)||memcmp(b+MS_MAGIC,step_code+MS_MAGIC,sizeof step_code-MS_MAGIC))return 0;
 uint64_t original;memcpy(&original,b+MS_ORIGINAL,8);
 if(original!=bm.lib+bm.profile->wrapper)return 0;
 uint32_t v;
 const unsigned fields[]={MS_ENABLED,MS_ACTIVE,MS_GATE,MS_DIRTY};
 for(unsigned i=0;i<4;i++){memcpy(&v,b+fields[i],4);if(v)return 0;}
 for(size_t i=sizeof step_code;i<1024;i++)if(b[i])return 0;
 return 1;
}
/* Read only the fixed profile reservation range. This records evidence after
 * a failed guard; it never makes an occupied range eligible for writing. */
static void bm_padding_report(void){
 const bm_profile *p=bm.profile;unsigned char bytes[1024];size_t used=0;
 if(!p||p->end<p->cave||p->end-p->cave>8*sizeof bytes)return;
 bm.slot_summary[0]=0;
 for(uint32_t off=p->cave;off+sizeof bytes<=p->end;off+=sizeof bytes){
  int count=0;
  if(bm_read(bm.base+off,bytes,sizeof bytes))count=-1;
  else for(size_t i=0;i<sizeof bytes;i++)if(bytes[i])count++;
  int n=snprintf(bm.slot_summary+used,sizeof bm.slot_summary-used,"%s0x%x:%d",used?",":"",off,count);
  if(n<0||(size_t)n>=sizeof bm.slot_summary-used)break;
  used+=(size_t)n;
 }
}
static int bm_reserve(uint64_t *out){
 const bm_profile *p=bm.profile;unsigned char b[1024];*out=0;
 /* padding is the file-backed text end, not the beginning of our allocation.
  * Other payloads can use the prefix before cave. We neither read it to claim
  * ownership nor modify it. Validate every actual reservation instead. */
 if(p->cave<p->padding||p->end<=p->cave||(p->cave&1023)||
    p->end!=((p->padding+0x3fffU)&~0x3fffU)||p->end-p->cave>8*sizeof b)
  return bm_fail("Invalid firmware reservation bounds",bm.base+p->cave);
 uint64_t reuse=0;
 for(uint32_t off=p->cave;off+sizeof b<=p->end;off+=sizeof b){
  if(bm_read(bm.base+off,b,sizeof b))return bm_fail("Launch-hook reservation unreadable",bm.base+off);
  size_t n=0;while(n<sizeof b&&!b[n])n++;
  if(n==sizeof b){if(!*out)*out=bm.base+off;}
  else if(!bm_retained(b)){
   bm_padding_report();
   return bm_fail("Reservation occupied, armed or unrecognized; reboot before retrying",bm.base+off);
  }
  /* the previous run's own retained slot: disarmed and no longer called, and that run is gone */
  else if(bm_reuse_allowed&&off==bm_reuse_slot)reuse=bm.base+off;
 }
 if(reuse){*out=reuse;return 0;}
 return *out?0:bm_fail("No fresh reservation remains; reboot before retrying",bm.base+p->cave);
}
static int bm_permissions(void){
 uint64_t pages[]={(bm.base+bm.profile->call)&~0x3fffULL,bm.cave&~0x3fffULL};
 const char *names[]={"call","reservation"};
 for(unsigned i=0;i<2;i++){
  int flags=kernel_get_vmem_protection(bm.pid,pages[i],0x4000);
  if(flags!=7){
   bm.check_address=pages[i];
   snprintf(bm.failure,sizeof bm.failure,"ShellCore %s page protection %d; requires RWX (7)",names[i],flags);return -1;
  }
 }
 return 0;
}
/* A manager that exits without Quit (killed, rest mode) leaves the call redirected to its armed shim. Adopt that
 * orphan only when it is ours (call into the profile reservation, our code, magic and original wrapper) and idle
 * with ShellCore stopped. When the previous run is confirmed gone (bm_reuse_allowed), keep the redirect and rewrite
 * the slot with the same code and a cleared mailbox (returns 1, attached). Otherwise disarm it and restore the call,
 * which leaves a normal retained slot and a fresh one is used. Anything else is left untouched for bm_valid. */
static int bm_recover(void){
 const bm_profile *p=bm.profile;unsigned char call[5],b[1024];int32_t rel;
 if(bm_read(bm.base+p->call,call,5)||call[0]!=0xe8||!memcmp(call,p->call_bytes,5))return 0;
 memcpy(&rel,call+1,4);
 uint64_t target=bm.base+p->call+5+(int64_t)rel;
 if(target<bm.base+p->cave||target+sizeof b>bm.base+p->end||((target-bm.base)&1023))return 0;
 uint64_t original=bm.lib+p->wrapper,code_original;
 if(bm_read(target,b,sizeof b)||memcmp(b,step_code,MS_ORIGINAL)||
    memcmp(b+MS_MAGIC,step_code+MS_MAGIC,sizeof step_code-MS_MAGIC))return 0;
 memcpy(&code_original,b+MS_ORIGINAL,8);if(code_original!=original)return 0;
 if(bm_stop())return bm_fail("Previous launch hook found; ShellCore could not be paused to recover it",target);
 bm_mailbox m;int rc=0;
 if(bm_read(target+MS_ENABLED,&m,sizeof m)||memcmp(m.magic,"NEOMS001",8))rc=bm_fail("Previous launch hook unreadable",target);
 else if(m.active||m.gate||m.dirty)rc=bm_fail("Previous launch hook busy; reboot required",target);
 else if(bm_reuse_allowed){
  unsigned char code[1024]={0};memcpy(code,step_code,sizeof step_code);
  memcpy(code+MS_ORIGINAL,&original,8);memcpy(code+MS_WAIT_CYCLES,&bm.cycles,8);
  bm.cave=target;memcpy(bm.saved,p->call_bytes,5);memcpy(bm.redirect,call,5);
  uint32_t zero=0;
  /* same code bytes; only the cycles and the mailbox change, so a thread stopped at the entry is unaffected */
  if(bm_permissions()||bm_store(target+MS_ENABLED,&zero,4)||bm_store(target,code,sizeof code))
   rc=bm_fail("Previous launch hook reuse failed",target);
  else{bm.attached=1;bm.recovered=2;}
 }
 else{
  uint32_t zero=0;
  if(bm_store(target+MS_ENABLED,&zero,4)||bm_store(bm.base+p->call,p->call_bytes,5)){
   /* put the redirect back: a disarmed shim only forwards to the original, an armed one is never left half-written */
   if(bm_store(bm.base+p->call,call,5))bm.fatal=1;
   rc=bm_fail("Previous launch hook recovery failed",target);
  }else bm.recovered=1;
 }
 if(bm_resume()){bm.fatal=1;bm.attached=0;return -1;}
 return rc?rc:bm.attached;
}
static int bm_install(uint32_t fw,uint64_t cycles){
 if(bm.attached)return 0;
 memset(&bm,0,sizeof bm);bm.profile=bm_find(fw);if(!bm.profile)return -1;
 bm.pid=find_proc("SceShellCore");if(bm.pid<0)return -2;
 dynlib_obj_t main={0},lib={0};uint32_t handle;
 if(kernel_dynlib_obj(bm.pid,0,&main)||kernel_dynlib_handle(bm.pid,"libkernel_sys.sprx",&handle)||
    kernel_dynlib_obj(bm.pid,handle,&lib)||main.mapsize<bm.profile->got+8)return -3;
 bm.base=main.mapbase;bm.lib=lib.mapbase;bm.cycles=cycles;
 bm.main_text_size=main.textsize;bm.main_map_size=main.mapsize;bm.frame_header=main.eh_frame_hdr;
 int recovered=bm_recover();
 if(recovered<0)return bm.fatal?-101:-4;
 if(recovered)return 0;
 if(bm_valid()||bm_reserve(&bm.cave)||bm_permissions())return -4;
 int64_t disp=(int64_t)bm.cave-(int64_t)(bm.base+bm.profile->call+5);if(disp!=(int32_t)disp)return -5;
 if(bm_stop())return -6;
 uint64_t selected=0;int rc=0;
 if(bm_valid()||bm_reserve(&selected)||selected!=bm.cave||bm_permissions())rc=-7;
 unsigned char code[1024]={0};memcpy(code,step_code,sizeof step_code);
 uint64_t original=bm.lib+bm.profile->wrapper;
 memcpy(code+MS_ORIGINAL,&original,8);memcpy(code+MS_WAIT_CYCLES,&cycles,8);
 memcpy(bm.saved,bm.profile->call_bytes,5);bm.redirect[0]=0xe8;int32_t rel=(int32_t)disp;memcpy(bm.redirect+1,&rel,4);
 if(!rc&&bm_store(bm.cave,code,sizeof code)){
  unsigned char zero[1024]={0};
  if(bm_store(bm.cave,zero,sizeof zero))bm.fatal=1;
  rc=-8;
 }
 if(!rc&&bm_store(bm.base+bm.profile->call,bm.redirect,5)){
  if(bm_store(bm.base+bm.profile->call,bm.saved,5)){bm.fatal=1;return -100;}
  rc=-9;
 }else if(!rc)bm.attached=1;
 if(bm_resume()){bm.fatal=1;return -101;}
 return rc;
}
static int bm_field(unsigned off,uint32_t value){return !bm.attached?-1:bm_store(bm.cave+off,&value,4);}
static int bm_mail(bm_mailbox *out){
 return !bm.attached||bm_read(bm.cave+MS_ENABLED,out,sizeof *out)||memcmp(out->magic,"NEOMS001",8)?-1:0;
}
static int bm_command(uint32_t command,uint32_t seq){
 return bm_field(MS_COMMAND,command)||bm_field(MS_RESPONSE_SEQ,seq)?-1:0;
}
static int bm_remove(void){
 if(!bm.attached)return bm.fatal?-1:0;
 bm_mailbox m;if(bm_mail(&m)||m.gate||m.dirty)return -1;
 if(bm_field(MS_ENABLED,0)||bm_stop())return -1;
 if(bm_mail(&m)||m.active||m.gate||m.dirty){
  if(bm_resume())bm.fatal=1;
  return -1;
 }
 unsigned char b[5];int rc=0;
 if(bm_read(bm.base+bm.profile->call,b,5))rc=-1;
 else if(!memcmp(b,bm.saved,5))bm.attached=0;
 else if(memcmp(b,bm.redirect,5))rc=-1;
 else if(bm_store(bm.base+bm.profile->call,bm.saved,5)&&bm_store(bm.base+bm.profile->call,bm.saved,5)){
  bm.fatal=1;return -100;
 }else bm.attached=0;
 if(bm_resume()){bm.fatal=1;return -101;}
 return rc;
}
