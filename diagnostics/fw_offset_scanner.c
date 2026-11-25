/* GPL-3.0-only. Firmware offset scanner: read-only capability probe.
 * Phase 0 of the firmware offset scanner design: it only reads, never writes,
 * never touches kernel TEXT, never attaches a debugger, and never prints
 * memory contents. Probe output is limited to rc/errno/value_hex of single
 * 8-byte DATA reads (one qword is a value, not a dump), and the NID search
 * reports presence and offset only.
 *
 * Modes are dispatched from argv: "--probe" runs the capability probe and is
 * also the DEFAULT when no arguments are given. prospero-deploy cannot pass
 * arguments to the payload (usage: prospero-deploy -h HOST PAYLOAD_PATH), so
 * a plain deploy must produce the probe output.
 *
 * Mode switch a deploy CAN trigger (prospero-deploy passes no argv): the
 * payload reads the flag file /data/fwscan/mode, one line holding
 * "data-live", "data-idle" or "data-write" (data-write = data-live plus one
 * mbd_apply()/mbd_restore() byte cycle; DEFAULT OFF, never reachable from
 * probe). An absent or unreadable file, or unknown content, falls back to the
 * read-only probe (fail-safe direction). Create the file on the console
 * before deploying, e.g.:
 *   ssh (or ftp) : mkdir -p /data/fwscan && echo data-live > /data/fwscan/mode
 * With argv, "--data <live|idle> [--expected-backing 0xOFF] [--write-test]"
 * selects the mode directly; --expected-backing is idle-only (pins the live
 * run's result when game state is gone) and --write-test is live-only.
 * "--shellcore" is reserved for a later phase.
 *
 * --data scans [data_base, data_base+64MiB) in 1 MiB kernel_copyout chunks
 * for the backing qword 0x100000000, translates candidate indices to DATA
 * offsets (chunk base + index*8, fws_backing_candidates contract), rebuilds
 * each layout via fws_layout_from_backing() and scores it through the
 * production mbd_read_snapshot()/mbd_eligible(). Output is field values
 * (qwords as value_hex), offsets, rc/errno and counts only - never raw
 * buffers, never kernel TEXT. Exit 0 when exactly one candidate survives,
 * 2 on wrong firmware, 3 when the surviving count is zero ("none") or greater
 * than one ("ambiguous"; idle with --expected-backing counts only the pinned
 * candidate), 4 when a requested write-test cycle does not come back clean.
 * "scan_end" is the DATA-relative offset the scan stopped at (0x4000000 when
 * complete; a chunk read failure stops early and is recorded there).
 *
 * Each line is an independent JSON object; keep stdout complete.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ps5/kernel.h>
#include "../src/dynlib.h" /* dynlib_obj_t/kernel_dynlib_obj, same calls as budget_mount_hook.h */
/* Production snapshot/eligibility (MBD_KREAD=kernel_copyout via the header's
 * default branch), then the pure derivation helpers (they need mbd_layout). */
#include "../src/neo_budget_data.h"
#include "fw_offset_scan.h"
#define FORMAT "neo-fwscan-v1"
#define SCAN_FW 0x13000000u
#define NID "HRDq-RDIGmI"
/* DATA qword probes, relative to KERNEL_ADDRESS_DATA_BASE (13.00 candidates
 * from the spec delta table; verified field by field before any write). */
#define PROBE_COUNT 3
static const uint32_t probe_offs[PROBE_COUNT]={0x2d55fb8u,0x2ed9fb8u,0x2e81fb8u};
static void string(const char*s){
 putchar('"');
 for(const unsigned char*p=(const unsigned char*)s;*p;p++){
  switch(*p){
  case '"':fputs("\\\"",stdout);break;case '\\':fputs("\\\\",stdout);break;
  case '\n':fputs("\\n",stdout);break;case '\r':fputs("\\r",stdout);break;case '\t':fputs("\\t",stdout);break;
  default:if(*p<32)printf("\\u%04x",*p);else putchar(*p);
  }
 }
 putchar('"');
}
static void begin(const char*type){printf("{\"format\":\"" FORMAT "\",\"record\":");string(type);}
static void hexq(uint64_t v){printf("\"0x%016lx\"",v);}
static void hexopt(int ok,uint64_t v){if(ok)hexq(v);else fputs("null",stdout);}
/* Same KERN_PROC_PROC enumeration as neo_diagnostic.c and shellcore_hook.h. */
static int find_proc(const char*comm){
 int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
 if(sysctl(mib,4,NULL,&size,NULL,0)||!size||size>32*1024*1024-0x10000)return -1;
 size+=0x10000;unsigned char*b=malloc(size);
 if(!b)return -1;
 int rc=sysctl(mib,4,b,&size,NULL,0),pid=-1;
 if(!rc){
  size_t off=0;
  while(off+sizeof(struct kinfo_proc)<=size){
   struct kinfo_proc*k=(void*)(b+off);
   if(k->ki_structsize<(int)sizeof(*k)||(size_t)k->ki_structsize>size-off)break;
   if(!strcmp(k->ki_comm,comm)){pid=k->ki_pid;break;}
   off+=(size_t)k->ki_structsize;
  }
 }
 free(b);return pid;
}
/* First occurrence of nid in [start,start+size) via userland process reads;
 * chunked with nlen-1 overlap so a match across a chunk boundary is found.
 * Returns the absolute address, or 0. Never prints the bytes read.
 * *failed_chunks counts chunks that could not be read (plus a malloc
 * failure), so a negative verdict is distinguishable from an unreadable map. */
static uint64_t nid_scan(int pid,uint64_t start,size_t size,const char*nid,unsigned*failed_chunks){
 size_t nlen=strlen(nid),chunk=1<<20;
 unsigned char*b=malloc(chunk+nlen);
 if(failed_chunks)*failed_chunks=0;
 if(!b||!nlen){free(b);if(failed_chunks)(*failed_chunks)++;return 0;}
 uint64_t found=0;
 for(size_t pos=0;pos<size&&!found;pos+=chunk){
  size_t len=size-pos<chunk+nlen-1?size-pos:chunk+nlen-1;
  if(len<nlen||kernel_proc_copyout(pid,(intptr_t)(start+pos),b,len)){
   if(len>=nlen&&failed_chunks)(*failed_chunks)++;
   continue;
  }
  for(size_t i=0;i+nlen<=len&&!found;i++)
   if(b[i]==(unsigned char)nid[0]&&!memcmp(b+i,nid,nlen))found=start+pos+i;
 }
 free(b);return found;
}
struct probe_result{int rc,error;uint64_t value;};
static int probe_mode(void){
 struct probe_result probes[PROBE_COUNT]={{0,0,0}};
 int status=1;const char*reason="firmware_query_failed";
 uint32_t fw=0;size_t fw_size=4;errno=0;
 int fw_rc=sysctlbyname("machdep.upd_version",&fw,&fw_size,NULL,0),fw_error=errno;
 int fw_printed=!fw_rc&&fw_size==4,fw_ok=fw_printed&&fw==SCAN_FW;
 uint64_t data_base=(uint64_t)KERNEL_ADDRESS_DATA_BASE;
 int shellcore_pid=-1,obj_rc=-1,handle_rc=-1,lib_rc=-1,probes_ok=0;
 uint32_t handle=0;dynlib_obj_t main={0},lib={0};
 uint64_t nid_off=0;unsigned nid_failed=0;int probed=0,shellcore_resolved=0,nid_present=0;
 /* Everything runs only on the matching firmware; otherwise stop per the
  * stop semantics and report wrong_firmware with exit 2. */
 if(fw_rc)status=1;
 else if(!fw_ok){reason="wrong_firmware";status=2;}
 else{
  reason=NULL;probed=1;
  for(unsigned i=0;i<PROBE_COUNT;i++){
   errno=0;
   probes[i].rc=kernel_copyout((intptr_t)(data_base+probe_offs[i]),&probes[i].value,8);
   probes[i].error=errno;
   probes_ok+=!probes[i].rc;
  }
  shellcore_pid=find_proc("SceShellCore");
  if(shellcore_pid>=0){
   errno=0;obj_rc=kernel_dynlib_obj(shellcore_pid,0,&main);
   shellcore_resolved=!obj_rc&&main.mapbase&&main.mapsize;
   if(shellcore_resolved){
    errno=0;handle_rc=kernel_dynlib_handle(shellcore_pid,"libkernel_sys.sprx",&handle);
    if(!handle_rc){errno=0;lib_rc=kernel_dynlib_obj(shellcore_pid,handle,&lib);}
    nid_off=nid_scan(shellcore_pid,main.mapbase,(size_t)main.mapsize,NID,&nid_failed);
    nid_present=nid_off!=0;
   }
  }
  if(probes_ok<1)reason="no_data_probes";
  else if(!shellcore_resolved)reason="shellcore_unresolved";
  else if(!nid_present)reason=nid_failed?"nid_scan_incomplete":"nid_absent";
  status=reason?1:0;
 }
 begin("session");printf(",\"mode\":\"probe\",\"firmware_raw\":");
 if(fw_printed)printf("\"0x%08x\"",fw);else fputs("null",stdout);
 printf(",\"firmware_rc\":%d,\"firmware_errno\":%d,\"firmware_bytes\":%zu,\"data_base\":",
        fw_rc,fw_error,fw_size);
 hexq(data_base);
 fputs(",\"probes\":[",stdout);
 for(unsigned i=0;i<PROBE_COUNT;i++){
  printf("%s{\"offset\":\"0x%lx\",\"rc\":",i?",":"",(unsigned long)probe_offs[i]);
  /* Probes only ran on the matching firmware; unexecuted reads are reported
   * as rc:-1/value_hex:null, never as successes. */
  if(probed)printf("%d,\"errno\":%d,\"value_hex\":",probes[i].rc,probes[i].error);
  else fputs("-1,\"errno\":0,\"value_hex\":",stdout);
  if(probed&&!probes[i].rc)hexq(probes[i].value);else fputs("null",stdout);
  putchar('}');
 }
 printf("],\"probes_ok\":%d,\"shellcore_pid\":",probes_ok);
 if(shellcore_pid>=0)printf("%d",shellcore_pid);else fputs("null",stdout);
 printf(",\"shellcore_obj_rc\":%d,\"mapbase\":",obj_rc);
 hexopt(shellcore_resolved,main.mapbase);
 fputs(",\"mapsize\":",stdout);hexopt(shellcore_resolved,main.mapsize);
 fputs(",\"textsize\":",stdout);hexopt(shellcore_resolved,main.textsize);
 printf(",\"libkernel_sys_handle_rc\":%d,\"libkernel_sys_handle\":",handle_rc);
 hexopt(!handle_rc,handle);
 printf(",\"libkernel_sys_obj_rc\":%d,\"libkernel_sys_base\":",lib_rc);
 hexopt(!lib_rc,lib.mapbase);
 fputs(",\"nid\":",stdout);string(NID);
 printf(",\"nid_present\":%s,\"nid_offset\":",nid_present?"true":"false");
 hexopt(nid_present,nid_off);
 printf(",\"nid_failed_chunks\":%u,\"probed\":%s,\"ok\":%s,\"reason\":",nid_failed,
        probed?"true":"false",status?"false":"true");
 if(reason)string(reason);else fputs("null",stdout);
 fputs("}\n",stdout);
 return status;
}
/* --data mode. MODE_PATH holds one line: data-live, data-idle or data-write
 * (see the header comment); absence, an unreadable file or unknown content
 * leaves the payload in the read-only probe default. */
#define MODE_PATH "/data/fwscan/mode"
#define SCAN_BYTES 0x4000000ULL /* 64 MiB */
#define CHUNK_BYTES ((size_t)1<<20)
static int read_mode_file(char*buf,size_t cap){
 FILE*f=fopen(MODE_PATH,"rb");
 if(!f)return 0;
 size_t n=fread(buf,1,cap-1,f);
 fclose(f);
 while(n&&(buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '||buf[n-1]=='\t'))n--;
 buf[n]=0;
 return 1;
}
/* Count of running "eboot.bin" processes; -1 when the enumeration fails.
 * Same bounded KERN_PROC_PROC enumeration as neo_diagnostic.c. */
static int count_games(void){
 int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t size=0;
 if(sysctl(mib,4,NULL,&size,NULL,0)||!size||size>32*1024*1024-0x10000)return -1;
 size+=0x10000;unsigned char*b=malloc(size);
 if(!b)return -1;
 int rc=sysctl(mib,4,b,&size,NULL,0),count=-1;
 if(!rc){
  count=0;size_t off=0;
  while(off+sizeof(struct kinfo_proc)<=size){
   struct kinfo_proc*k=(void*)(b+off);
   if(k->ki_structsize<(int)sizeof(*k)||(size_t)k->ki_structsize>size-off)break;
   if(!strcmp(k->ki_comm,"eboot.bin"))count++;
   off+=(size_t)k->ki_structsize;
  }
 }
 free(b);return count;
}
static void print_layout(const mbd_layout*L){
 printf("{\"fw\":\"0x%08x\",\"backing_base\":\"0x%lx\",\"container_ptr\":\"0x%lx\","
        "\"ext_global\":\"0x%lx\",\"references\":\"0x%lx\",\"profile\":\"0x%lx\","
        "\"maximum\":\"0x%lx\"}",L->fw,(unsigned long)L->backing_base,
        (unsigned long)L->container_ptr,(unsigned long)L->ext_global,
        (unsigned long)L->references,(unsigned long)L->profile,
        (unsigned long)L->maximum);
}
static void print_snapshot(const mbd_snapshot*s){
 printf("{\"extension\":");hexq(s->extension);
 fputs(",\"container\":",stdout);hexq(s->container);
 printf(",\"references\":\"0x%lx\",\"profile\":\"0x%lx\"",
        (unsigned long)s->references,(unsigned long)s->profile);
 fputs(",\"maximum\":",stdout);hexq(s->maximum);
 fputs(",\"backing_base\":",stdout);hexq(s->backing_base);
 fputs(",\"available\":",stdout);hexq(s->available);
 printf(",\"field38\":");hexq(s->field38);
 fputs(",\"reserved\":",stdout);hexq(s->reserved);
 printf(",\"app_id\":\"0x%lx\",\"category\":\"0x%lx\",\"mode\":\"0x%lx\",\"flags\":\"0x%lx\"}",
        (unsigned long)s->app_id,(unsigned long)s->category,
        (unsigned long)s->mode,(unsigned long)s->flags);
}
static int data_mode(const char*state,int write_test,uint64_t expected_backing){
 int status=1;const char*reason="firmware_query_failed";
 uint32_t fw=0;size_t fw_size=4;errno=0;
 int fw_rc=sysctlbyname("machdep.upd_version",&fw,&fw_size,NULL,0),fw_error=errno;
 int fw_ok=!fw_rc&&fw_size==4&&fw==SCAN_FW;
 uint64_t data_base=(uint64_t)KERNEL_ADDRESS_DATA_BASE;
 int games=count_games();
 if(fw_rc)status=1;
 else if(!fw_ok){reason="wrong_firmware";status=2;}
 else{reason=NULL;status=0;}
 begin("state");printf(",\"mode\":\"data\",\"state\":");string(state);
 printf(",\"firmware_raw\":");
 if(!fw_rc&&fw_size==4)printf("\"0x%08x\"",fw);else fputs("null",stdout);
 printf(",\"firmware_rc\":%d,\"firmware_errno\":%d,\"data_base\":",fw_rc,fw_error);
 hexq(data_base);
 printf(",\"scan_bytes\":\"0x%lx\",\"chunk_bytes\":\"0x%lx\",\"expected_backing\":",
        (unsigned long)SCAN_BYTES,(unsigned long)CHUNK_BYTES);
 if(expected_backing)printf("\"0x%lx\"",(unsigned long)expected_backing);
 else fputs("null",stdout);
 printf(",\"game_count\":");
 if(games>=0)printf("%d",games);else fputs("null",stdout);
 fputs("}\n",stdout);
 unsigned candidate_count=0,eligible_count=0,idle_count=0;
 int scan_complete=0,pinned_ok=0,scan_truncated=0;
 uint64_t scan_end=0;unsigned long long qwords=0;
 mbd_layout elig_layout;mbd_snapshot elig_snap;int have_elig=0;int write_ran=0;
 if(!status){
  unsigned char*raw=malloc(CHUNK_BYTES);
  uint64_t*hits=malloc(CHUNK_BYTES);
  /* Worst case for one chunk: every qword matches, one index each. */
  uint32_t*idx=malloc(CHUNK_BYTES/8*sizeof *idx);
  scan_complete=raw&&hits&&idx;
  if(scan_complete){
   scan_end=SCAN_BYTES;
   for(uint64_t off=0;off<SCAN_BYTES;off+=CHUNK_BYTES){
    if(kernel_copyout((intptr_t)(data_base+off),raw,CHUNK_BYTES)){
     scan_complete=0;scan_end=off;break;
    }
    memcpy(hits,raw,CHUNK_BYTES);
    /* fws_backing_candidates reports EVERY 0x100000000 hit in the chunk;
     * a negative return means the idx buffer truncated (unreachable at this
     * size, kept as a guard) and fails the scan instead of a clean verdict. */
    int n=fws_backing_candidates(hits,CHUNK_BYTES/8,idx,CHUNK_BYTES/8);
    qwords+=CHUNK_BYTES/8;
    if(n<0){scan_complete=0;scan_truncated=1;scan_end=off;break;}
    for(int i=0;i<n;i++){
     uint32_t backing_off=(uint32_t)(off+(uint64_t)idx[i]*8);
     mbd_layout L=fws_layout_from_backing(SCAN_FW,backing_off);
     mbd_snapshot s;
     errno=0;
     int read_rc=mbd_read_snapshot(&L,&s);
     int eligible=!read_rc&&mbd_eligible(&s);
     /* Idle acceptance: stock extension/maximum and the backing qword still
      * 0x100000000 at this layout (the container is 0 when idle, so the
      * snapshot is not mbd_eligible by design). */
     uint64_t backing_q=0;
     int idle_ok=!read_rc&&!kernel_copyout((intptr_t)(data_base+L.backing_base),&backing_q,8)
                 &&!s.extension&&!s.maximum&&backing_q==0x100000000ULL;
     candidate_count++;
     begin("candidate");printf(",\"mode\":\"data\",\"state\":");string(state);
     printf(",\"backing_off\":\"0x%lx\",\"layout\":",(unsigned long)backing_off);
     print_layout(&L);
     fputs(",\"snapshot\":",stdout);
     if(read_rc)printf("{\"read_rc\":%d,\"errno\":%d}",read_rc,errno);
     else{fputs("{\"read_rc\":0,",stdout);print_snapshot(&s);putchar('}');}
     printf(",\"eligible\":%s,\"idle_ok\":%s}\n",eligible?"true":"false",
            idle_ok?"true":"false");
     if(eligible){eligible_count++;if(!have_elig){have_elig=1;elig_layout=L;elig_snap=s;}}
     if(idle_ok)idle_count++;
     if(expected_backing&&backing_off==expected_backing)pinned_ok=idle_ok;
    }
   }
  }
  free(raw);free(hits);free(idx);
  /* Live verdict: exactly one eligible candidate. Idle verdict: exactly one
   * candidate at the pinned layout passing the idle criteria (or, unpinned,
   * exactly one idle_ok candidate). */
  unsigned surviving=!strcmp(state,"idle")
      ?(unsigned)(expected_backing?pinned_ok:idle_count)
      :eligible_count;
  const char*verdict=NULL;
  if(!surviving)verdict=scan_complete?"none":"scan_incomplete";
  else if(surviving>1)verdict="ambiguous";
  if(scan_truncated)verdict="scan_truncated"; /* a truncated scan is never clean */
  if(verdict){reason=verdict;status=3;}
  /* Write-test: DEFAULT OFF; only after a clean live run, one apply/restore
   * byte cycle through the production transaction code (MBD_KWRITE). */
  if(write_test&&status==0&&have_elig){
   write_ran=1;
   mbd_transaction tx;memset(&tx,0,sizeof tx);
   int rc=mbd_apply(&elig_layout,&tx,&elig_snap);
   int restore_rc=mbd_restore(&elig_layout,&tx);
   uint64_t readback=0;errno=0;
   int rb_rc=mbd_tx_read_global(&elig_layout,&readback);
   begin("write_test");printf(",\"mode\":\"data\",\"state\":");string(state);
   printf(",\"backing_off\":\"0x%lx\",\"rc\":%d,\"readback\":",
          (unsigned long)elig_layout.backing_base,rc);
   if(!rb_rc)hexq(readback);else fputs("null",stdout);
   printf(",\"restored\":%s,\"restore_rc\":%d,\"timing\":\"mid_session_not_production\"}\n",
          tx.restored?"true":"false",restore_rc);
   if(rc||!tx.restored||restore_rc||rb_rc||readback)status=4;
  }
 }
 begin("end");printf(",\"mode\":\"data\",\"state\":");string(state);
 printf(",\"scan_complete\":%s,\"scan_end\":\"0x%lx\",\"qwords_scanned\":%llu,"
        "\"candidate_count\":%u,\"eligible_count\":%u,\"idle_count\":%u,"
        "\"write_test_ran\":%s,\"reason\":",scan_complete?"true":"false",
        scan_end,qwords,candidate_count,eligible_count,idle_count,
        write_ran?"true":"false");
 if(reason)string(reason);else fputs("null",stdout);
 printf(",\"ok\":%s}\n",status?"false":"true");
 return status;
}

int main(int argc,char**argv){
 setvbuf(stdout,NULL,_IONBF,0);
 const char*mode=argc>1?argv[1]:NULL;
 if(mode&&!strcmp(mode,"--probe"))return probe_mode();
 if(mode&&!strcmp(mode,"--data")){
  const char*st=argc>2?argv[2]:NULL;
  const char*eb=NULL;int write_test=0;
  for(int i=3;i<argc;i++){
   if(!strcmp(argv[i],"--write-test"))write_test=1;
   else if(!strcmp(argv[i],"--expected-backing")&&i+1<argc)eb=argv[++i];
   else{begin("error");printf(",\"mode\":\"--data\",\"reason\":\"bad_args\"}\n");return 1;}
  }
  uint64_t expected=eb?(uint64_t)strtoull(eb,NULL,0):0;
  if(eb&&(!expected||!st||strcmp(st,"idle"))){
   /* --expected-backing pins the idle verdict to the live run's result. */
   begin("error");printf(",\"mode\":\"--data\",\"reason\":\"bad_args\"}\n");return 1;
  }
  if(st&&!strcmp(st,"live"))return data_mode("live",write_test,0);
  if(st&&!strcmp(st,"idle")){
   if(write_test){begin("error");printf(",\"mode\":\"--data\",\"reason\":\"bad_state\"}\n");return 1;}
   return data_mode("idle",0,expected);
  }
  begin("error");printf(",\"mode\":\"--data\",\"reason\":\"bad_state\"}\n");return 1;
 }
 if(mode){
  begin("error");printf(",\"mode\":");string(mode);
  fputs(",\"reason\":\"unknown_mode\"}\n",stdout);
  return 1;
 }
 /* No argv: the deployable path. The flag file selects the mode; anything
  * else (absent file, unknown content) stays on the read-only probe. */
 char flag[32];
 if(read_mode_file(flag,sizeof flag)){
  if(!strcmp(flag,"data-live"))return data_mode("live",0,0);
  if(!strcmp(flag,"data-idle"))return data_mode("idle",0,0);
  if(!strcmp(flag,"data-write"))return data_mode("live",1,0);
 }
 return probe_mode();
}
