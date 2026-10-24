/* Host-only: simulated MP1 backend, no console connection. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NEO_GPU_TEST 1
static unsigned clock_mhz,writes,last_set;
static int supported,read_fail,set_fail,restore_fail;
static int gpu_backend_supported(void){return supported;}
static int gpu_backend_read(unsigned*out){if(read_fail||!supported)return -1;*out=clock_mhz;return 0;}
static int gpu_backend_set(unsigned v){
 writes++;last_set=v;
 if((set_fail&&v>=1800)||(restore_fail&&v==900))return -1;
 clock_mhz=v;return 0;
}
#include "../src/gpu_control.h"
#include "../src/settings.h"
static void reset(void){
 gpu=(gpu_control){.target=GPU_DEFAULT_MHZ};clock_mhz=900;writes=last_set=0;
 supported=1;read_fail=set_fail=restore_fail=0;
}
int main(void){
 reset();assert(!gpu_poll(44)&&!writes&&!strcmp(gpu_state_text(),"disabled"));
 assert(gpu_request(1,2200,0)==1&&!writes&&gpu.enabled&&!gpu.applied);
 clock_mhz=1800;assert(!gpu_poll(44)&&writes==1&&last_set==2200&&gpu.baseline==1800);
 clock_mhz=2210;assert(!gpu_poll(44)&&writes==1&&gpu.actual==2210); /* Do not fight SMU. */
 assert(!gpu_request(1,2150,44)&&writes==2&&gpu.baseline==1800);
 assert(!gpu_request(0,2200,44)&&last_set==1800&&!gpu.enabled&&!gpu.applied);
 assert(!gpu_request(0,2200,44)&&writes==3); /* Idempotent off. */
 /* Extended range: 2250, 2300 and 2350 are now valid targets. */
 reset();assert(gpu_request(1,2250,0)==1&&gpu.target==2250&&!writes);
 reset();assert(gpu_request(1,2300,0)==1&&gpu.target==2300&&!writes);
 reset();assert(gpu_request(1,2350,0)==1&&gpu.target==2350&&!writes);
 reset();assert(gpu_request(1,2201,44)<0&&!writes&&!gpu.enabled);
 assert(gpu_request(1,2351,44)<0&&!writes);assert(gpu_request(1,2400,44)<0&&!writes);assert(gpu_request(1,1799,44)<0&&!writes);
 supported=0;assert(gpu_request(1,2200,44)<0&&!writes);
 assert(gpu_poll(44)<0&&!writes&&!strcmp(gpu_state_text(),"unavailable"));
 gpu_configure(1,2200);assert(!gpu.enabled&&!writes); /* Saved enable ignored on unsupported firmware. */
 reset();read_fail=1;assert(gpu_request(1,2200,44)<0&&!writes);
 reset();set_fail=1;assert(gpu_request(1,2200,44)==-2&&writes==2&&clock_mhz==900);
 assert(gpu.blocked&&!gpu.applied);gpu_poll(44);assert(writes==2);
 set_fail=0;assert(!gpu_request(1,2200,44)&&writes==3&&gpu.baseline==900);
 restore_fail=1;assert(gpu_request(0,2200,44)==-3&&gpu.restore_pending&&gpu.baseline_valid);
 assert(gpu_request(1,2200,44)==-3);assert(!strcmp(gpu_state_text(),"restore failed"));
 restore_fail=0;assert(!gpu_request(0,2200,44)&&!gpu.restore_pending&&clock_mhz==900);
 reset();assert(!gpu_request(1,2200,44));unsigned n=writes;
 clock_mhz=900;assert(gpu_poll(0)==1&&writes==n&&!gpu.baseline_valid); /* System takes over on exit. */
 clock_mhz=1800;assert(!gpu_poll(45)&&writes==n+1&&gpu.baseline==1800&&gpu.owner_pid==45);
 assert(gpu_poll(-1)==1&&writes==n+1); /* Unknown game state never issues SET. */
 assert(!gpu_request(0,2200,45));
 reset();clock_mhz=1800;assert(!gpu_request(1,2200,44));
 for(unsigned i=0;i<GPU_MAX_RECOVERIES;i++){
  unsigned before=writes;clock_mhz=1800;
  assert(!gpu_poll(44)&&writes==before);
  assert(!gpu_poll(44)&&writes==before+1&&last_set==2200&&gpu.baseline==1800);
 }
 clock_mhz=1800;assert(!gpu_poll(44));assert(gpu_poll(44)==-2&&gpu.blocked);
 unsigned stopped=writes;assert(gpu_poll(44)==-2&&writes==stopped);
 assert(!gpu_request(0,2200,44)&&last_set==1800);
 /* Recoveries are given back once the boost has held for GPU_STABLE_POLLS polls
  * (separate dashboard visits must not add up to a lockout). */
 reset();clock_mhz=1800;assert(!gpu_request(1,2200,44));
 for(unsigned i=0;i<2;i++){clock_mhz=1800;assert(!gpu_poll(44));assert(!gpu_poll(44));}
 assert(gpu.recoveries==2);
 for(unsigned i=0;i<GPU_STABLE_POLLS;i++){clock_mhz=2200;assert(!gpu_poll(44));}
 assert(gpu.recoveries==0&&!gpu.blocked);
 reset();clock_mhz=1800;assert(!gpu_request(1,2200,44));unsigned before=writes;
 clock_mhz=1800;assert(!gpu_poll(44));assert(gpu_poll(-1)==1);
 assert(!gpu_poll(44)&&writes==before); /* Unknown scan breaks reset evidence. */
 clock_mhz=2155;assert(!gpu_poll(44)&&writes==before);
 unsigned mhz=0;
 for(unsigned m=1800;m<=GPU_MAX_MHZ;m+=50){char b[32];snprintf(b,sizeof b,"mhz=%u",m);assert(gpu_form_mhz(b,&mhz)==1&&mhz==m);}
 const char*bad[]={"mhz=2400","mhz=2200&mhz=1800","mhz=22000","mhz=2200x","mhz=-1","mhz=%32%33%35%30","mhz=","mhz=999999999999999999999","mhz=2349"};
 for(unsigned i=0;i<sizeof bad/sizeof*bad;i++)assert(gpu_form_mhz(bad[i],&mhz)==-1);
 assert(gpu_form_mhz("other=2200",&mhz)==0);
 neo_settings c;assert(neo_settings_parse("gpu_clock_enabled=1\ngpu_clock_mhz=2400\n",&c)<0&&!c.gpu_enabled);assert(!neo_settings_parse("neo=0\nmemory_8gib=1\n",&c));
 assert(!c.neo&&c.memory&&!c.gpu_enabled&&c.gpu_mhz==2200);
 assert(!neo_settings_parse("neo=1\nmemory_8gib=1\ngpu_clock_enabled=1\ngpu_clock_mhz=2200\n",&c));
 char text[128];assert(neo_settings_format(&c,text,sizeof text)>0);
 FILE*f=tmpfile();assert(f&&fputs(text,f)>=0&&!fflush(f));rewind(f);
 char disk[128]={0};assert(fread(disk,1,sizeof disk-1,f)>0);fclose(f);
 neo_settings d;assert(!neo_settings_parse(disk,&d)&&d.neo&&d.memory&&d.gpu_enabled&&d.gpu_mhz==2200);
 reset();gpu_configure(d.gpu_enabled,d.gpu_mhz);assert(!writes&&gpu_poll(0)==1);
 clock_mhz=1800;assert(!gpu_poll(44)&&last_set==2200); /* Restored settings apply to next game. */
 assert(neo_settings_parse("gpu_clock_enabled=1\ngpu_clock_mhz=999999\n",&c)<0&&!c.gpu_enabled);
 assert(neo_settings_parse("gpu_clock_enabled=10\ngpu_clock_mhz=2200\n",&c)<0&&!c.gpu_enabled);
 assert(!neo_settings_parse("gpu_clock_enabled=1\r\ngpu_clock_mhz=2200\r\n",&c)&&c.gpu_enabled);
 assert(neo_settings_format(&c,text,5)<0);
 puts("PASS GPU lifecycle, 2350 cap, no repeated SET, rollback, restoration, form validation and saved-settings restart");return 0;
}
