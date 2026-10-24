/* GPL-3.0-only. PS4-only clock policy using the stock MP1 driver.
 * Apply on request/new game, with bounded recovery of sustained 1800 MHz resets.
 * Never chase ordinary SMU variation. Runs in the manager's HTTP thread.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define GPU_MIN_MHZ 1800U
#define GPU_MAX_MHZ 2350U      /* was 2200U; extended for PS5 Pro testing */
#define GPU_DEFAULT_MHZ 2200U  /* default stays at the tested value */
#define GPU_MAX_RECOVERIES 10U /* automatic re-applies allowed before giving up */
#define GPU_STABLE_POLLS 10U   /* polls above 1800 MHz before the recovery budget is refunded */
static int gpu_target_valid(unsigned mhz){return mhz>=GPU_MIN_MHZ&&mhz<=GPU_MAX_MHZ&&mhz%50==0;}
#ifndef NEO_GPU_TEST
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
static int gpu_backend_supported(void){
 return 1;
}
static int gpu_backend_read(unsigned*out){
 if(!gpu_backend_supported())return -1;
 int fd=open("/dev/mp1",O_RDONLY);if(fd<0)return -1;
 uint32_t values[27]={0};int rc=ioctl(fd,0x406cae01UL,values);close(fd);
 if(rc||values[21]<400||values[21]>2380)return -1;
 *out=values[21];return 0;
}
static int gpu_backend_set(unsigned mhz){
 if(!gpu_backend_supported()||mhz<400||mhz>2380)return -1;
 int fd=open("/dev/mp1",O_RDWR);if(fd<0)return -1;
 uint32_t value=mhz;int rc=ioctl(fd,0x8004ae17UL,&value);close(fd);return rc;
}
#endif
typedef struct{
 int enabled,applied,owner_pid,baseline_valid,blocked,restore_pending,reading_valid;
 unsigned target,baseline,actual,actions,reset_samples,recoveries,stable_samples;
 char message[192];
} gpu_control;
static gpu_control gpu={.target=GPU_DEFAULT_MHZ,.message="GPU boost disabled"};
static void gpu_configure(int enable,unsigned mhz){
 gpu.enabled=!!enable&&gpu_backend_supported()&&gpu_target_valid(mhz);
 gpu.target=gpu_target_valid(mhz)?mhz:GPU_DEFAULT_MHZ;
}
static int gpu_refresh(void){
 unsigned actual=0;gpu.reading_valid=!gpu_backend_read(&actual);
 if(gpu.reading_valid)gpu.actual=actual;
 return gpu.reading_valid?0:-1;
}
static int gpu_restore(void){
 if(!gpu.baseline_valid){gpu.applied=0;gpu.restore_pending=0;return 0;}
 gpu.actions++;
 if(gpu_backend_set(gpu.baseline)){
  gpu.blocked=1;gpu.restore_pending=1;
  snprintf(gpu.message,sizeof gpu.message,"Clock restoration failed. Retry Disable before enabling boost.");return -3;
 }
 unsigned previous=gpu.baseline;
 gpu.baseline_valid=0;gpu.applied=0;gpu.owner_pid=0;gpu.restore_pending=0;gpu.blocked=0;
 gpu_refresh();
 snprintf(gpu.message,sizeof gpu.message,"Boost off; previous clock requested: %u MHz",previous);return 0;
}
static void gpu_owner_changed(int pid){
 if(pid>=0&&gpu.owner_pid&&pid!=gpu.owner_pid&&!gpu.restore_pending){
  /* A launch/exit changes the system power policy. Do not replay the old game's
   * baseline over the new system policy; take a new baseline for the new game. */
  gpu.baseline_valid=0;gpu.applied=0;gpu.owner_pid=0;gpu.blocked=0;
  gpu.reset_samples=0;gpu.recoveries=0;gpu.stable_samples=0;
 }
}
static int gpu_poll(int pid){
 if(!gpu_backend_supported()){
  gpu.reading_valid=0;snprintf(gpu.message,sizeof gpu.message,"GPU clock control unavailable");return -1;
 }
 gpu_owner_changed(pid);
 if(gpu.restore_pending)return -3;
 if(gpu.blocked)return -2;
 if(gpu_refresh()){
  gpu.reset_samples=0;
  snprintf(gpu.message,sizeof gpu.message,"SMU clock query failed; no new clock request sent");return -1;
 }
 if(!gpu.enabled){snprintf(gpu.message,sizeof gpu.message,"GPU boost disabled");return 0;}
 if(pid<=0){
  gpu.reset_samples=0;
  snprintf(gpu.message,sizeof gpu.message,pid<0?"Game detection unavailable; waiting":"Enabled; waiting for a PS4 game");return 1;
 }
 /* Two consecutive policy polls at exactly the standard 1800 MHz profile.
  * This is a reset heuristic, not a foreground or thermal-state measurement.
  * At most GPU_MAX_RECOVERIES automatic recoveries per game/manual enable; never retry a
  * failed SET, failed read, or ambiguous process scan. */
 if(gpu.applied&&gpu.target>1800&&gpu.actual==1800){
  gpu.stable_samples=0;
  if(++gpu.reset_samples>=2){
   gpu.reset_samples=0;
   if(gpu.recoveries>=GPU_MAX_RECOVERIES){
    gpu.blocked=1;
    snprintf(gpu.message,sizeof gpu.message,"Repeated 1800 MHz resets; automatic recovery stopped. Check game/system state before retrying Enable.");
    return -2;
   }
   gpu.recoveries++;gpu.applied=0;
  }
 }else{
  gpu.reset_samples=0;
  /* Boost held for a while: earlier resets were separate events (for example
   * dashboard visits), so give the recovery budget back. Rapid back-to-back
   * resets never reach GPU_STABLE_POLLS and still hit the limit above. */
  if(gpu.applied&&gpu.actual>1800){
   if(++gpu.stable_samples>=GPU_STABLE_POLLS){gpu.recoveries=0;gpu.stable_samples=0;}
  }else gpu.stable_samples=0;
 }
 if(!gpu.applied){
  if(!gpu.baseline_valid){gpu.baseline=gpu.actual;gpu.baseline_valid=1;gpu.owner_pid=pid;}
  gpu.actions++;
  if(gpu_backend_set(gpu.target)){
   int restored=gpu_restore();gpu.blocked=1;
   if(!restored)snprintf(gpu.message,sizeof gpu.message,"Boost request failed; previous clock restored. Retry Enable.");
   return restored?-3:-2;
  }
  gpu.applied=1;gpu_refresh();
 }
 if(gpu.reading_valid)snprintf(gpu.message,sizeof gpu.message,"Target %u MHz requested for the running PS4 game",gpu.target);
 else snprintf(gpu.message,sizeof gpu.message,"Target %u MHz requested; SMU reading unavailable",gpu.target);
 return 0;
}
static int gpu_request(int enable,unsigned mhz,int pid){
 if(enable&&(!gpu_target_valid(mhz)||!gpu_backend_supported()))return -1;
 gpu_owner_changed(pid);
 if(!enable){gpu.enabled=0;return gpu_restore();}
 if(gpu.restore_pending){snprintf(gpu.message,sizeof gpu.message,"Retry Disable to restore the previous clock first");return -3;}
 gpu.enabled=1;gpu.target=mhz;gpu.blocked=0;gpu.applied=0;gpu.reset_samples=0;gpu.recoveries=0;gpu.stable_samples=0;
 return gpu_poll(pid);
}
static const char*gpu_state_text(void){
 if(!gpu_backend_supported())return "unavailable";
 if(gpu.restore_pending)return "restore failed";
 if(gpu.blocked)return "request failed";
 if(!gpu.enabled)return "disabled";
 return gpu.applied?"enabled":"waiting";
}
/* Accept exactly one unescaped decimal MHz field from a small HTML form. */
static int gpu_form_mhz(const char*body,unsigned*out){
 int found=0;const char*p=body;
 while(*p){
  const char*end=strchr(p,'&');if(!end)end=p+strlen(p);
  if((size_t)(end-p)>=4&&!memcmp(p,"mhz=",4)){
   if(found||end==p+4)return -1;
   unsigned value=0;
   for(const char*q=p+4;q<end;q++){if(*q<'0'||*q>'9'||value>10000)return -1;value=value*10+(unsigned)(*q-'0');}
   if(!gpu_target_valid(value))return -1;
   *out=value;found=1;
  }
  p=*end?end+1:end;
 }
 return found;
}
