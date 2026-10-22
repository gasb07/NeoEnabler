/* Host controller sequencing tests. No console or network. */
#define NEO_DATA_CONTROLLER_TEST
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <stdarg.h>
#define MBD_DELTA 0xc0000000ULL
#define MBD_EXPECTED_STOCK 0x18c000000ULL
#define MBD_TX_RECOVERY 3
#define MS_DIRTY 1
#define MS_GATE 2
#define MB_UNKNOWN -1
#define MB_STOCK 0
#define MB_8GIB 1
typedef struct {int state,access_ready;char detail[200];} mb_snapshot;
typedef struct {int dummy;} mbd_layout;
typedef struct {int touched,owned,restored,failed;uint64_t previous;} mbd_transaction;
typedef struct {uint64_t container,field38,maximum,extension;uint32_t app_id,category,references,flags,profile,mode;} mbd_snapshot;
typedef struct {uint32_t seq,phase,category,gate,active,dirty;int32_t result;char title[12];} bm_mailbox;
static struct {int attached,fatal,recovered;char failure[160];} bm;
static char memory_message[256];static int memory_target;
static mbd_layout layout;static mbd_snapshot snap;static bm_mailbox mail;
static int eligible,apply_rc,restore_rc,apply_calls,restore_calls,mutations,remove_calls,held,command_fail;
static char events[128];static unsigned event_count;
static void event(char c){events[event_count++]=c;events[event_count]=0;}
static int mb_check(mb_snapshot*s){(void)s;return 0;}
static int memory_text_poll(void){return 0;}
static int memory_text_request(int e){return e;}
static int mb_games(void){return 0;}
static void log_line(const char *f,...){(void)f;}
static int usleep(unsigned u){(void)u;return 0;}
static const mbd_layout *mbd_find_layout(uint32_t fw){(void)fw;return &layout;}
static int bm_install(uint32_t fw,uint64_t c){(void)fw;(void)c;bm.attached=1;return 0;}
static int bm_mail(bm_mailbox*m){*m=mail;return 0;}
static int bm_command(uint32_t c,uint32_t s){assert(s==mail.seq);event('0'+c);if(command_fail)return -1;if(c==1&&held){mail.phase=2;mail.gate=1;mail.active=1;}return 0;}
static int bm_field(unsigned off,uint32_t value){if(off==MS_DIRTY){event(value?'D':'d');mail.dirty=value;}else {assert(off==MS_GATE);event('g');assert(restore_calls>0);mail.gate=value;}return 0;}
static int bm_remove(void){remove_calls++;return 0;}
static int mbd_tx_read_global(const mbd_layout*l,uint64_t*e){(void)l;*e=0;return 0;}
static int mbd_read_snapshot(const mbd_layout*l,mbd_snapshot*s){(void)l;*s=snap;return 0;}
static int mbd_eligible(const mbd_snapshot*s){(void)s;return eligible;}
static int mbd_apply(const mbd_layout*l,mbd_transaction*t,const mbd_snapshot*s){(void)l;(void)s;event('A');apply_calls++;assert(mail.phase==2&&mail.gate&&mail.active&&mail.dirty);if(!apply_rc){t->touched=t->owned=1;t->restored=0;mutations++;snap.extension=MBD_DELTA;}return apply_rc;}
static int mbd_restore(const mbd_layout*l,mbd_transaction*t){(void)l;event('R');restore_calls++;if(!restore_rc){t->owned=0;t->restored=1;}return restore_rc;}
#include "../src/budget_mount_control.h"
static int wants(const char*t){assert(!strcmp(t,"CUSA01111"));return memory_target;}
static int refused;
static void refuse(const char*t){assert(!strcmp(t,"CUSA01111"));refused++;}
static void reset(void){
 memset(&bm,0,sizeof bm);bm.attached=1;memset(&mail,0,sizeof mail);mail.seq=1;mail.phase=1;mail.category=0xf000;memcpy(mail.title,"CUSA01111",10);
 memset(&snap,0,sizeof snap);snap.container=0xffff800000001000ULL;snap.app_id=42;snap.category=0xf000;snap.references=1;snap.profile=1;snap.mode=2;
 memset(&memory_transaction,0,sizeof memory_transaction);memset(&memory_owner,0,sizeof memory_owner);
 memory_data=&layout;memory_data_mode=1;memory_constructor=memory_data_blocked=0;memory_seq=0;memory_target=1;memory_wants8=wants;memory_refused8=refuse;refused=0;
 eligible=held=1;apply_rc=restore_rc=apply_calls=restore_calls=mutations=remove_calls=command_fail=0;event_count=0;events[0]=0;
}
int main(void){
 reset();memory_data_tick();assert(!strcmp(events,"1DA3"));assert(mutations==1&&memory_transaction.owned);
 memory_data_tick();assert(apply_calls==1); /* outstanding transaction blocks a second launch */
 snap.flags=0x40;snap.field38=MBD_EXPECTED_STOCK+MBD_DELTA;memory_data_tick();assert(memory_constructor&&!restore_calls);
 snap.references=0;snap.container=0;mail.active=0;memory_data_tick();assert(!strcmp(events,"1DA3Rdg"));assert(!memory_transaction.owned);
 assert(!refused);   /* applied: its 8 GiB patches stay on */
 /* A launch that wants 8 GiB and is released without it reports the title, so its 8 GiB patches are turned off. */
 reset();held=0;memory_data_tick();assert(!mutations&&!strcmp(events,"12")&&refused==1);
 reset();eligible=0;memory_data_tick();assert(!mutations&&!strcmp(events,"12")&&refused==1);
 reset();apply_rc=2;memory_data_tick();assert(!mutations&&refused==1);
 reset();mail.result=-1;memory_data_tick();assert(!mutations&&!strcmp(events,"2")&&!refused);
 reset();mail.phase=4;memory_data_tick();assert(!mutations&&!strcmp(events,"2")&&!refused);
 reset();memory_target=0;memory_data_tick();memory_data_tick();assert(!mutations&&!strcmp(events,"2")&&!refused); /* duplicate sequence */
 reset();memory_data_tick();snap.app_id++;memory_data_tick();assert(!restore_calls&&memory_data_blocked&&memory_transaction.owned&&mail.gate);
 reset();memory_data_tick();snap.flags=0x40;snap.field38=1;memory_data_tick();assert(!restore_calls&&memory_data_blocked&&memory_transaction.owned&&mail.gate);
 reset();assert(memory_shutdown()==0&&remove_calls==1);
 reset();mail.active=1;assert(memory_shutdown()==-1&&!remove_calls);
 reset();mail.gate=1;assert(memory_shutdown()==-1&&!remove_calls);
 reset();memory_data_tick();assert(memory_shutdown()==-1&&!remove_calls);
 reset();memory_data_tick();restore_rc=1;snap.container=0;snap.references=0;mail.active=0;memory_data_tick();assert(memory_data_blocked&&strchr(events,'g')==NULL);
 reset();command_fail=1;memory_data_tick();assert(memory_data_blocked&&!mutations&&refused==1);
 /* A deadline cannot authorize rollback inside the mount callback. */
 reset();memory_data_tick();memory_applied=time(NULL)-31;memory_data_tick();
 assert(memory_data_blocked&&!restore_calls&&memory_transaction.owned&&mail.gate);
 snap.container=0;snap.references=0;memory_data_tick();assert(!restore_calls); /* callback active */
 mail.active=0;memory_data_tick();assert(restore_calls==1&&memory_data_blocked&&!memory_transaction.owned);
 assert(!strcmp(events,"1DA3Rdg"));
 /* Neither partial teardown nor an active callback authorizes rollback. */
 reset();memory_data_tick();snap.container=0;mail.active=0;memory_data_tick();assert(!restore_calls);
 reset();memory_data_tick();snap.references=0;mail.active=0;memory_data_tick();assert(!restore_calls);
 reset();memory_data_tick();snap.container=0;snap.references=0;memory_data_tick();assert(!restore_calls);
 /* Every changed owner guard retains the transaction and gate. */
 for(int guard=0;guard<6;guard++){
  reset();memory_data_tick();
  switch(guard){case 0:snap.references=2;break;case 1:snap.profile++;break;
   case 2:snap.mode++;break;case 3:snap.category++;break;case 4:snap.maximum=1;break;
   case 5:snap.extension=0;break;}
  memory_data_tick();assert(memory_data_blocked&&!restore_calls&&memory_transaction.owned&&mail.gate);
 }
 /* Preserve the preflight reason in the user-visible memory detail. */
 reset();bm.attached=0;strcpy(bm.failure,"BeginAppMount2 call differs at +0x1");
 mb_snapshot detail;assert(memory_check(&detail)==-1);
 assert(strstr(detail.detail,"BeginAppMount2 call differs"));
 puts("PASS: DATA controller HELD sequencing, refusal, constructor, rollback order, duplicate/outstanding transaction, shutdown, identity, timeout retention and quiescent recovery");
 return 0;
}
