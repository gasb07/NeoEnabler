/* GPL-3.0-only. Single-threaded controller for the existing DATA transaction.
 * No kernel TEXT calls on the DATA path. Release the mount only after HELD,
 * and retain serialization until the owned global has been restored.
 */
#pragma once
#ifndef NEO_DATA_CONTROLLER_TEST
#include "neo_budget_data.h"
#include "budget_mount_hook.h"
#endif
static const mbd_layout *memory_data;
static mbd_transaction memory_transaction;
static mbd_snapshot memory_owner;
static int memory_data_mode,memory_constructor,memory_data_blocked;
static uint32_t memory_seq;
static time_t memory_applied;
static int (*memory_wants8)(const char*);
/* A launch that wants 8 GiB is released without it: its 8 GiB patches must be off before ps-patch-system reads them. */
static void (*memory_refused8)(const char*);
static void memory_refuse(const char *title){if(memory_refused8)memory_refused8(title);}

static int memory_check(mb_snapshot *s){
 if(!memory_data_mode)return mb_check(s);
 memset(s,0,sizeof *s);s->state=MB_UNKNOWN;
 if(!bm.attached||memory_data_blocked){
  snprintf(s->detail,sizeof s->detail,"No-HV launch hook unavailable%s%s%s",bm.failure[0]?": ":"",bm.failure,memory_data_blocked?"; reboot required":"");return -1;
 }
 uint64_t ext;
 if(mbd_tx_read_global(memory_data,&ext)||(ext&&(!memory_transaction.owned||ext!=MBD_DELTA))){
  snprintf(s->detail,sizeof s->detail,"Unknown DATA extension; nothing written");return -1;
 }
 s->state=memory_transaction.owned&&!memory_transaction.restored?MB_8GIB:MB_STOCK;
 s->access_ready=1;
 snprintf(s->detail,sizeof s->detail,"%s",s->state==MB_8GIB?"8GB Mode DATA transaction active (no HV)":"No-HV 8GB Mode ready for the next eligible Pro launch");
 return 0;
}
static int memory_init(uint32_t fw,uint64_t cycles){
 memory_data=mbd_find_layout(fw);
 memory_data_mode=memory_data!=NULL;
 if(!memory_data_mode)return 0;
 int rc=bm_install(fw,cycles);
 snprintf(memory_message,sizeof memory_message,"No-HV launch hook %s (rc=%d)%s%s%s",rc?"unavailable":"installed",rc,rc&&bm.failure[0]?": ":"",rc?bm.failure:"",
          bm.recovered==2?"; hook left by the previous run reused":bm.recovered?"; hook left by a previous run recovered":"");
 if(bm.fatal)memory_data_blocked=1;
 return rc;
}
static int memory_data_restore(void){
 int rc=mbd_restore(memory_data,&memory_transaction);
 if(rc){memory_data_blocked=1;snprintf(memory_message,sizeof memory_message,"DATA rollback failed; do not launch games; reboot required");return 6;}
 /* Restore global first, then clear dirty, then let another mount acquire. */
 if(bm_field(MS_DIRTY,0)||bm_field(MS_GATE,0)){
  memory_data_blocked=1;snprintf(memory_message,sizeof memory_message,"DATA restored but launch gate cleanup failed; reboot required");return 6;
 }
 memory_constructor=0;memset(&memory_owner,0,sizeof memory_owner);
 snprintf(memory_message,sizeof memory_message,"Original DATA extension restored; no-HV 8GB Mode ready");
 log_line("8GB Mode DATA: rollback verified; launch gate released");return 0;
}
static int memory_poll(void){
 if(!memory_data_mode)return memory_text_poll();
 return memory_data_blocked?6:bm.attached?0:1;
}
static int memory_request(int enable){
 if(!memory_data_mode)return memory_text_request(enable);
 if(memory_data_blocked)return 6;
 memory_target=!!enable;
 snprintf(memory_message,sizeof memory_message,"%s",enable?"No-HV 8GB Mode requested for eligible Pro launches":
          memory_transaction.owned?"8GB Mode stays active until the game closes; restoration pending":"No-HV 8GB Mode disabled for future launches");
 return bm.attached?0:1;
}
static void memory_data_tick(void){
 if(!memory_data_mode||!bm.attached)return;
 if(memory_data_blocked){
  /* Recovery requires both budget teardown and a quiescent retained shim.
   * A deadline alone never authorizes rollback while a constructor may run. */
  if(memory_transaction.owned&&!memory_transaction.restored){
   mbd_snapshot s;bm_mailbox m;
   if(!mbd_read_snapshot(memory_data,&s)&&!s.container&&!s.references&&mb_games()==0&&
      !bm_mail(&m)&&!m.active){
    memory_data_restore();
    snprintf(memory_message,sizeof memory_message,"DATA restored after teardown; further writes blocked; reboot required");
   }
  }
  return;
 }
 if(memory_transaction.owned&&!memory_transaction.restored){
  mbd_snapshot s;
  if(mbd_read_snapshot(memory_data,&s)){
   if(!memory_constructor&&time(NULL)-memory_applied>30){
    memory_data_blocked=1;
    snprintf(memory_message,sizeof memory_message,"DATA snapshot unreadable at constructor deadline; gate retained; reboot required");
   }
   return;
  }
  if(!s.container||!s.references){
   bm_mailbox m;
   if(!s.container&&!s.references&&mb_games()==0&&!bm_mail(&m)&&!m.active)memory_data_restore();
   return;
  }
  if(s.container!=memory_owner.container||s.app_id!=memory_owner.app_id){
   memory_data_blocked=1;
   snprintf(memory_message,sizeof memory_message,"DATA container identity changed; gate retained; reboot required");
   log_line("8GB Mode DATA: container identity changed; rollback awaits quiescent teardown");return;
  }
  if(s.references!=1||s.profile!=memory_owner.profile||s.mode!=memory_owner.mode||
     s.category!=memory_owner.category||s.maximum||s.extension!=MBD_DELTA){
   memory_data_blocked=1;
   snprintf(memory_message,sizeof memory_message,"DATA owner guards changed; gate retained; reboot required");return;
  }
  if(!memory_constructor&&(s.flags&0x40)){
   memory_constructor=1;
   log_line("8GB Mode DATA: constructor app=%u field38=0x%lx expected=0x%lx match=%d",
     s.app_id,(unsigned long)s.field38,(unsigned long)(MBD_EXPECTED_STOCK+MBD_DELTA),s.field38==MBD_EXPECTED_STOCK+MBD_DELTA);
   if(s.field38!=MBD_EXPECTED_STOCK+MBD_DELTA){
    memory_data_blocked=1;
    snprintf(memory_message,sizeof memory_message,"Unexpected constructor budget; gate retained; reboot required");return;
   }
   snprintf(memory_message,sizeof memory_message,"8GB Mode constructor verified (no HV); extension held until game closes");
  }
  if(!memory_constructor&&time(NULL)-memory_applied>30){
   log_line("8GB Mode DATA: constructor deadline; gate and transaction retained until quiescent teardown");
   memory_data_blocked=1;
   snprintf(memory_message,sizeof memory_message,"Constructor deadline; DATA ownership retained; reboot required");
  }
  return;
 }
 bm_mailbox m;if(bm_mail(&m)||!m.seq||m.seq==memory_seq)return;
 memory_seq=m.seq;
 char title[11]={0};memcpy(title,m.title,10);
 int target=m.result>=0&&m.category==0xf000&&m.phase==1&&memory_wants8&&memory_wants8(title);
 if(!target){bm_command(2,m.seq);return;}
 /* Mark dirty BEFORE touching DATA; it also prevents accidental detach. */
 if(bm_command(1,m.seq)){memory_data_blocked=1;memory_refuse(title);return;}
 uint32_t seq=m.seq;int held=0;
 for(int i=0;i<10000;i++){
  if(bm_mail(&m))break;
  if(m.seq!=seq)break;
  if(m.phase==2&&m.gate==1&&m.active>0){held=1;break;}
  if(m.phase>=4)break;
  usleep(500);
 }
 if(!held){bm_command(2,seq);log_line("8GB Mode DATA: HELD not confirmed for %s; no write",title);memory_refuse(title);return;}
 mbd_snapshot s;int rc=mbd_read_snapshot(memory_data,&s);
 if(rc||!mbd_eligible(&s)||s.category!=m.category){
  bm_command(2,seq);log_line("8GB Mode DATA: launch %s refused by snapshot guards (read=%d)",title,rc);memory_refuse(title);return;
 }
 if(bm_field(MS_DIRTY,1)){bm_command(2,seq);memory_data_blocked=1;memory_refuse(title);return;}
 rc=mbd_apply(memory_data,&memory_transaction,&s);
 log_line("8GB Mode DATA: launch=%s seq=%u HELD apply=%d owned=%d app=%u",title,seq,rc,memory_transaction.owned,s.app_id);
 if(rc){
  memory_refuse(title);
  if(rc==MBD_TX_RECOVERY){memory_data_blocked=1;return;}
  bm_field(MS_DIRTY,0);bm_command(2,seq);return;
 }
 memory_owner=s;memory_constructor=0;memory_applied=time(NULL);
 if(bm_command(3,seq)){
  /* A RUNNING write may have taken effect despite a failed readback. Keep
   * serialization and recovery ownership; never release an uncertain mount. */
  memory_data_blocked=1;
  snprintf(memory_message,sizeof memory_message,"Launch acknowledgment failed; DATA transaction retained; reboot required");
  return;
 }
 snprintf(memory_message,sizeof memory_message,"8GB Mode DATA applied to %s (no HV); waiting for constructor",title);
}
static int memory_shutdown(void){
 if(!memory_data_mode)return 0;
 if(memory_data_blocked||(memory_transaction.owned&&!memory_transaction.restored))return -1;
 bm_mailbox m;if(bm.attached&&(bm_mail(&m)||m.active||m.gate||m.dirty))return -1;
 return bm_remove();
}
