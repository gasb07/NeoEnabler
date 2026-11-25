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
 * "data-live", "data-idle", "data-write" (data-write = data-live plus one
 * mbd_apply()/mbd_restore() byte cycle; DEFAULT OFF, never reachable from
 * probe) or "shellcore" (BeginAppMount2 profile discovery). An absent or
 * unreadable file, or unknown content, falls back to the read-only probe
 * (fail-safe direction). Create the file on the console before deploying,
 * e.g.:
 *   ssh (or ftp) : mkdir -p /data/fwscan && echo data-live > /data/fwscan/mode
 * With argv, "--data <live|idle> [--expected-backing 0xOFF] [--write-test]"
 * selects the mode directly; --expected-backing is idle-only (pins the live
 * run's result when game state is gone) and --write-test is live-only.
 *
 * --shellcore derives the BeginAppMount2 bm_profile (function, call, import_,
 * got, wrapper, stacks, bytes) for THIS firmware and dry-runs every bm_valid()
 * byte check read-only before anything would ever be patched. Records:
 * session (resolution + NID lookups + strategy + ambiguity sites), profile
 * (full tuple + padding_source + bm_valid_dry_run) and end. Exit 0 only when
 * a unique profile passes every byte check and fws_reserve_bounds_ok; 2 on
 * wrong firmware; 3 on ambiguity (the sites are listed in the session
 * record); 1 on any other failure (reason names it). Nothing is ever
 * written: all reads go through kernel_proc_copyout / kernel_copyout.
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
#include "fw_record.h" /* candidate/snapshot JSONL emitters (host-tested) */
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
     int read_errno=errno;
     int eligible=!read_rc&&mbd_eligible(&s);
     /* Idle acceptance: stock extension/maximum and the backing qword still
      * 0x100000000 at this layout (the container is 0 when idle, so the
      * snapshot is not mbd_eligible by design). */
     uint64_t backing_q=0;
     int idle_ok=!read_rc&&!kernel_copyout((intptr_t)(data_base+L.backing_base),&backing_q,8)
                 &&!s.extension&&!s.maximum&&backing_q==0x100000000ULL;
     candidate_count++;
     fws_print_candidate(stdout,state,backing_off,&L,read_rc,read_errno,&s,
                         eligible,idle_ok);
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

/* ============ --shellcore mode: BeginAppMount2 profile discovery ============
 * The brief's primary strategy (NID HRDq-RDIGmI -> SCE relocation walk) is
 * adjusted by the Task-3 findings: the NID string is ABSENT from the
 * ShellCore main map on 13.00 (proven full-map scan, nid_failed_chunks:0), so
 * no main-map relocation can name it. Stage A therefore searches BOTH maps:
 *  - nid_reloc: NID string in the MAIN map dynstr -> find the JUMP_SLOT
 *    relocation for that symbol -> its r_offset is the GOT slot.
 *  - nid_export: NID string in the libkernel_sys dynstr -> the export's
 *    st_value IS the wrapper offset; the GOT slot is then the unique
 *    main-map qword equal to lib+wrapper.
 * Stage B (documented fallback, the expected 13.00 path): scan the main map
 * for `ff 25 rel32` PLT stubs whose rip+6+rel GOT slot holds a pointer into
 * libkernel_sys whose 20-byte prologue is FWS_PROLOGUE16+imm32 (wrapper_stack
 * DERIVED from those bytes, never hardcoded). Uniqueness is enforced through
 * the call graph: the winning stub must be the only candidate with e8 rel32
 * callers, and exactly one caller (the profile's call). Any Stage A
 * absence/ambiguity falls through to Stage B; a Stage B ambiguity exits 3
 * with the sites listed. padding is NOT read from main.textsize unchecked:
 * Task 3 measured kernel-ptr-like garbage there (struct fields after mapsize
 * may have shifted), so textsize is used only when plausible, with
 * rx_tail_scan / zero_run_scan as documented fallbacks (padding_source
 * records the method). Every bm_valid() byte check is re-run read-only
 * against the derived profile; nothing is written. */

/* Read whole maps into buffers once: the scans below are byte-granular and
 * would otherwise issue thousands of per-hit copyouts. */
static unsigned char*sc_read_map_all(int pid,uint64_t base,size_t size,unsigned*failed){
 *failed=1;
 unsigned char*b=malloc(size);
 if(!b)return NULL;
 for(size_t off=0;off<size;off+=CHUNK_BYTES){
  size_t len=size-off<CHUNK_BYTES?size-off:CHUNK_BYTES;
  if(kernel_proc_copyout(pid,(intptr_t)(base+off),b+off,len)){free(b);return NULL;}
 }
 *failed=0;return b;
}
/* First occurrence of nid in a map buffer: 1 found (*off = byte index),
 * 0 absent. */
static int sc_buf_find(const unsigned char*b,size_t size,const char*nid,uint64_t*off){
 size_t nlen=strlen(nid);
 if(size<nlen)return 0;
 for(size_t i=0;i+nlen<=size;i++)
  if(b[i]==(unsigned char)nid[0]&&!memcmp(b+i,nid,nlen)){*off=i;return 1;}
 return 0;
}
/* d_ptr values are vaddrs for a loaded image; accept already-absolute values
 * (at/above base) too and normalize both to a buffer index. */
static uint64_t sc_dec(uint64_t v,uint64_t base){return v>=base?v-base:v;}
typedef struct {
 uint64_t strtab,symtab,strsz,jmprel,pltrelsz; /* buffer indexes */
} sc_dyn;
/* In-memory ELF64 PT_DYNAMIC parse of a mapped image buffer. Returns 0 when
 * the tags needed below were found with sane shapes. */
static int sc_parse_dyn(const unsigned char*buf,size_t size,uint64_t base,sc_dyn*d){
 memset(d,0,sizeof*d);
 if(size<64||memcmp(buf,"\x7f""ELF",4)||buf[4]!=2)return -1;
 uint64_t phoff;memcpy(&phoff,buf+0x20,8);
 uint16_t phentsize,phnum;memcpy(&phentsize,buf+0x36,2);memcpy(&phnum,buf+0x38,2);
 if(!phoff||phentsize<0x38||!phnum||phnum>32||phoff+(uint64_t)phnum*phentsize+0x38>size)return -1;
 uint64_t dyn=0;int have=0;
 for(unsigned i=0;i<phnum;i++){
  const unsigned char*ph=buf+phoff+(size_t)i*phentsize;
  uint32_t ptype;memcpy(&ptype,ph,4);
  if(ptype==2){memcpy(&dyn,ph+0x10,8);have=1;break;}
 }
 if(!have)return -1;
 uint64_t dyn_idx=sc_dec(dyn,base);
 if(dyn_idx+16>size)return -1;
 for(unsigned i=0;dyn_idx+16*(uint64_t)(i+1)<=size&&i<1024;i++){
  uint64_t tag,val;memcpy(&tag,buf+dyn_idx+16*i,8);memcpy(&val,buf+dyn_idx+16*i+8,8);
  if(tag==0)break;
  switch(tag){
  case 5:d->strtab=sc_dec(val,base);break;
  case 6:d->symtab=sc_dec(val,base);break;
  case 10:d->strsz=val;break;
  case 23:d->jmprel=sc_dec(val,base);break;
  case 2:d->pltrelsz=val;break;
  default:break;
  }
 }
 if(!d->strtab||!d->symtab||!d->strsz)return -1;
 if(d->strtab+d->strsz>size)return -1;
 return 0;
}
typedef struct {uint64_t st_value;uint32_t sym_index;} sc_symhit;
/* Dynamic symbols whose st_name indexes the string `name` and whose
 * st_value is nonzero. Returns the hit count (capped, *trunc signals
 * overflow); -1 when the symtab/strtab shapes are unusable. */
static int sc_find_syms_by_name(const unsigned char*buf,size_t size,const sc_dyn*d,
                                const char*name,sc_symhit*out,int cap,int*trunc){
 size_t nlen=strlen(name);
 *trunc=0;
 if(d->symtab>=d->strtab)return -1;
 uint64_t end=d->strtab<size?d->strtab:size;
 if(end>d->symtab+24ULL*65536)end=d->symtab+24ULL*65536;
 int count=0;
 for(uint64_t off=d->symtab;off+24<=end;off+=24){
  uint32_t st_name;memcpy(&st_name,buf+off,4);
  if(!st_name||st_name+nlen>=d->strsz)continue;
  if(memcmp(buf+d->strtab+st_name,name,nlen)||buf[d->strtab+st_name+nlen])continue;
  uint64_t st_value;memcpy(&st_value,buf+off+8,8);
  if(!st_value)continue;
  if(count==cap){*trunc=1;break;}
  out[count].st_value=st_value;
  out[count].sym_index=(uint32_t)((off-d->symtab)/24);
  count++;
 }
 return count;
}
/* Stage A nid_reloc: the JUMP_SLOT (R_X86_64_JUMP_SLOT, r_info type 6)
 * relocation whose symbol names `name`. Returns -1 unusable, 0 none,
 * 1 unique (*got_off = GOT slot buffer index), 2 ambiguous (sites listed). */
static int sc_got_by_name(const unsigned char*buf,size_t size,const sc_dyn*d,
                          const char*name,uint64_t base,uint64_t*got_off,
                          uint64_t*sites,unsigned*site_count,unsigned*site_trunc){
 sc_symhit hits[8];int trunc=0;
 int n=sc_find_syms_by_name(buf,size,d,name,hits,8,&trunc);
 if(n<0)return -1;
 if(!n)return 0;
 if(n>1||trunc)return 2;
 uint32_t idx=hits[0].sym_index;
 if(!d->jmprel||!d->pltrelsz)return -1;
 int found=0;
 for(uint64_t off=d->jmprel;off+24<=size&&off+24<=d->jmprel+d->pltrelsz;off+=24){
  uint64_t r_offset,r_info;memcpy(&r_offset,buf+off,8);memcpy(&r_info,buf+off+8,8);
  if((uint32_t)r_info!=6||(uint32_t)(r_info>>32)!=idx)continue;
  if(found<16)sites[(*site_count)++]=sc_dec(r_offset,base);
  else *site_trunc=1;
  *got_off=sc_dec(r_offset,base);
  found++;
 }
 return found==1?1:found?2:0;
}
/* PLT-stub candidates: `ff 25 rel32` whose rip+6+rel GOT slot (main-map
 * buffer index) holds a pointer into libkernel_sys whose 20-byte prologue is
 * FWS_PROLOGUE16+imm32; wrapper_stack comes from those bytes. Returns 0, or
 * -1 when more than cap candidates exist (*trunc; ambiguous by definition,
 * never silently dropped). */
typedef struct {
 uint32_t stub_off,got_off;
 uint64_t got_value;
 uint32_t wrapper,wrapper_stack;
 uint32_t call_off[4];
 unsigned calls;
} sc_stub;
#define SC_MAX_STUBS 64
static int sc_scan_stubs(const unsigned char*mb,size_t msize,const unsigned char*lb,
                         size_t lsize,uint64_t lib_base,sc_stub*out,int cap,
                         int*count,int*trunc){
 *count=0;*trunc=0;
 for(uint64_t i=0;i+6<=msize;i++){
  if(mb[i]!=0xff||mb[i+1]!=0x25)continue;
  int32_t rel;memcpy(&rel,mb+i+2,4);
  int64_t target=(int64_t)i+6+rel;
  if(target<0||target+8>(int64_t)msize)continue;
  uint64_t got_value;memcpy(&got_value,mb+target,8);
  if(got_value<lib_base||got_value-lib_base+20>(uint64_t)lsize)continue;
  const unsigned char*w=lb+(size_t)(got_value-lib_base);
  if(memcmp(w,FWS_PROLOGUE16,FWS_PROLOGUE_LEN))continue;
  if(*count==cap){*trunc=1;return -1;}
  uint32_t ws;memcpy(&ws,w+16,4);
  out[*count].stub_off=(uint32_t)i;
  out[*count].got_off=(uint32_t)target;
  out[*count].got_value=got_value;
  out[*count].wrapper=(uint32_t)(got_value-lib_base);
  out[*count].wrapper_stack=ws;
  out[*count].call_off[0]=0;out[*count].call_off[1]=0;
  out[*count].call_off[2]=0;out[*count].call_off[3]=0;
  out[*count].calls=0;
  (*count)++;
 }
 return 0;
}
/* e8 rel32 callers per stub candidate: the profile's call is the unique
 * caller of the import stub. Bounded to [0,bound): callers live in text,
 * which lies below the PLT in this layout (13.20: call 0x6d7c11 <
 * import_ 0x195b690; GOT/data above) - scanning the whole map would count
 * stray e8 bytes in data as callers. */
static void sc_scan_calls(const unsigned char*mb,uint64_t bound,size_t msize,
                          sc_stub*stubs,int count){
 for(uint64_t i=0;i+5<=bound;i++){
  if(mb[i]!=0xe8)continue;
  int32_t rel;memcpy(&rel,mb+i+1,4);
  int64_t target=(int64_t)i+5+rel;
  if(target<0||target>=(int64_t)msize)continue;
  for(int k=0;k<count;k++){
   if((uint64_t)stubs[k].stub_off!=(uint64_t)target)continue;
   if(stubs[k].calls<4)stubs[k].call_off[stubs[k].calls]=(uint32_t)i;
   stubs[k].calls++;
  }
 }
}
/* Nearest 16-byte prologue occurrence at or before `before` (the containing
 * function's entry) plus the total count of occurrences scanned. 0 when none. */
static uint64_t sc_nearest_prologue(const unsigned char*mb,uint64_t before,
                                    uint64_t*total){
 uint64_t nearest=0;*total=0;
 for(uint64_t i=0;i+FWS_PROLOGUE_LEN<=before;i++){
  if(mb[i]!=0x55||memcmp(mb+i,FWS_PROLOGUE16,FWS_PROLOGUE_LEN))continue;
  nearest=i;(*total)++;
 }
 return nearest;
}
/* ELF diagnostics: is there an ELF64 header at the map base, and how many
 * program headers? (SCE images may not expose a standard PT_DYNAMIC at the
 * map base; recorded so a dead Stage A is explainable, not silent.) */
static int sc_ehdr_phnum(const unsigned char*buf,size_t size,uint16_t*phnum){
 *phnum=0;
 if(size<64||memcmp(buf,"\x7f""ELF",4)||buf[4]!=2)return 0;
 memcpy(phnum,buf+0x38,2);
 return 1;
}
/* Main-map qwords equal to `value` (Stage A nid_export GOT-slot search).
 * Returns the count (capped, *trunc signals overflow). */
static int sc_find_qword(const unsigned char*mb,size_t msize,uint64_t value,
                         uint64_t*out,int cap,int*trunc){
 *trunc=0;int count=0;
 for(uint64_t i=0;i+8<=msize;i++){
  uint64_t v;memcpy(&v,mb+i,8);
  if(v!=value)continue;
  if(count==cap){*trunc=1;break;}
  out[count++]=i;
 }
 return count;
}
typedef struct {int pid;} sc_read_ctx;
static int sc_read_mem(void*ctx,uint64_t addr,void*buf,size_t n){
 return kernel_proc_copyout(((sc_read_ctx*)ctx)->pid,(intptr_t)addr,buf,n);
}
/* padding derivation (Task-3 adjusted), in priority order:
 *  - "textsize": only when main.textsize reads plausible (< mapsize,
 *    > 0x10000). On 13.00 it reads kernel-ptr-like garbage, so this is the
 *    exception path, never the plan.
 *  - "rx_tail_scan": walk the executable mapping (16 KiB pages, exec bit, the
 *    bm_permissions granularity) up to rx_end, then scan the map buffer back
 *    page-by-page from rx_end for the last non-zero 4 KiB page and, inside
 *    it, the last non-zero byte; padding = that byte + 1. Text is followed by
 *    zero padding to the segment page end, so [padding,rx_end) is zero by
 *    construction.
 *  - "zero_run_scan": coarse fallback - the first 4 KiB page after 0x10000
 *    that is all zero with its successor also zero; padding = page start
 *    (may overstate the text end by up to one page; bounds stay valid).
 * Every result must lie past the discovered call, inside the map, and pass
 * fws_reserve_bounds_ok() before it may reach the profile. */
static int sc_derive_padding(int pid,uint64_t base,uint64_t mapsize,uint64_t textsize,
                             const unsigned char*mb,uint64_t call_off,
                             uint32_t*padding,const char**source,
                             uint64_t*rx_end_out,int*rx_walked){
 *rx_walked=0;*rx_end_out=0;
 uint32_t cand=0;const char*src=NULL;
 if(textsize>0x10000&&textsize<mapsize){cand=(uint32_t)textsize;src="textsize";}
 if(!src){
  uint64_t rx_end=0;
  for(uint64_t off=0;off+0x4000<=mapsize;off+=0x4000){
   int flags=kernel_get_vmem_protection(pid,(intptr_t)(base+off),0x4000);
   if(flags<0||!(flags&4))break;
   rx_end=off+0x4000;
  }
  *rx_end_out=rx_end;*rx_walked=1;
  if(rx_end>=0x10000&&rx_end<=mapsize){
   uint64_t off=rx_end-0x4000;
   for(;;){
    const unsigned char*page=mb+off;
    int nz=0;
    for(uint64_t j=0;j<0x4000;j++)if(page[j]){nz=1;break;}
    if(nz){
     uint64_t last=0;
     for(int64_t j=0x3fff;j>=0;j--)if(page[j]){last=(uint64_t)j;break;}
     cand=(uint32_t)(off+last+1);src="rx_tail_scan";
     break;
    }
    if(!off)break;
    off-=0x4000;
   }
  }
 }
 if(!src){
  for(uint64_t off=0x10000;off+0x8000<=mapsize;off+=0x4000){
   int zero=1;
   for(uint64_t j=0;j<0x8000&&zero;j++)if(mb[off+j])zero=0;
   if(zero){cand=(uint32_t)off;src="zero_run_scan";break;}
  }
 }
 if(!src)return -1;
 if(cand<=call_off||cand>=mapsize)return -1;
 uint32_t e=fws_end(cand),c=fws_cave(cand,e);
 if(e>mapsize||!fws_reserve_bounds_ok(cand,c,e))return -1;
 *padding=cand;*source=src;
 return 0;
}
/* Unique import stub for a Stage A GOT slot: exactly one candidate stub
 * targets got_off AND has exactly one e8 caller (0 none, 1 unique, 2
 * ambiguous). */
static int sc_import_for_got(const sc_stub*stubs,int count,uint64_t got_off,sc_stub*out){
 const sc_stub*hit=NULL;int hits=0;
 for(int k=0;k<count;k++){
  if((uint64_t)stubs[k].got_off!=got_off||!stubs[k].calls)continue;
  if(stubs[k].calls>1)return 2;
  if(hits)return 2;
  hit=&stubs[k];hits++;
 }
 if(!hit)return 0;
 *out=*hit;
 return 1;
}
static int shellcore_mode(void){
 int status=1;const char*reason="firmware_query_failed";
 uint32_t fw=0;size_t fw_size=4;errno=0;
 int fw_rc=sysctlbyname("machdep.upd_version",&fw,&fw_size,NULL,0),fw_error=errno;
 int fw_ok=!fw_rc&&fw_size==4&&fw==SCAN_FW;
 if(fw_rc)status=1;
 else if(!fw_ok){reason="wrong_firmware";status=2;}
 else{reason=NULL;status=0;}
 /* Everything below runs only on the matching firmware. */
 int pid=-1,obj_rc=-1,handle_rc=-1,lib_rc=-1,resolved=0,lib_resolved=0;
 dynlib_obj_t main_obj={0},lib={0};uint32_t handle=0;
 unsigned char*mb=NULL,*lb=NULL;
 int main_dyn_ok=0,lib_dyn_ok=0,nid_main_found=0,nid_lib_found=0;
 uint64_t nid_main_off=0,nid_lib_off=0;
 int main_ehdr_ok=0,lib_ehdr_ok=0;uint16_t main_phnum=0,lib_phnum=0;
 const char*strategy=NULL;
 uint64_t got_off=0,wrapper=0,import_off=0,call_off=0,function_off=0;
 uint32_t function_stack=0,wrapper_stack=0;
 unsigned char call_bytes[5]={0},stub_bytes[6]={0};
 uint32_t padding=0,pend=0,pcave=0;
 const char*padding_source=NULL;
 int padding_ok=0,reserve_ok=0,dryrun_pass=0;
 const char*failing_check=NULL;
 uint64_t rx_end=0;int rx_walked=0,rx_printed=0;
 uint64_t prologue_total=0;
 int have_profile=0;
 uint64_t sites[16];unsigned site_count=0,site_trunc=0;
 fws_profile P;memset(&P,0,sizeof P);
 if(!reason){
  pid=find_proc("SceShellCore");
  if(pid>=0){
   errno=0;obj_rc=kernel_dynlib_obj(pid,0,&main_obj);
   if(!obj_rc&&main_obj.mapbase&&main_obj.mapsize){
    resolved=1;
    errno=0;handle_rc=kernel_dynlib_handle(pid,"libkernel_sys.sprx",&handle);
    if(!handle_rc){
     errno=0;lib_rc=kernel_dynlib_obj(pid,handle,&lib);
     lib_resolved=!lib_rc&&lib.mapbase&&lib.mapsize;
    }
   }
  }
  if(!resolved)reason="shellcore_unresolved";
  else if(!lib_resolved)reason="libkernel_sys_unresolved";
 }
 sc_stub stubs[SC_MAX_STUBS];int stub_count=0,stub_trunc=0;
 if(!reason){
  unsigned fm=0,fl=0;
  mb=sc_read_map_all(pid,main_obj.mapbase,(size_t)main_obj.mapsize,&fm);
  if(fm)reason="main_map_read_failed";
  else{
   lb=sc_read_map_all(pid,lib.mapbase,(size_t)lib.mapsize,&fl);
   if(fl)reason="lib_map_read_failed";
  }
 }
 if(!reason){
  /* NID lookups in BOTH maps (Task 3: absent from the main map on 13.00;
   * the libkernel_sys export table is the remaining Stage A anchor). */
  nid_main_found=sc_buf_find(mb,(size_t)main_obj.mapsize,NID,&nid_main_off);
  nid_lib_found=sc_buf_find(lb,(size_t)lib.mapsize,NID,&nid_lib_off);
  sc_dyn dm,dl;
  main_ehdr_ok=sc_ehdr_phnum(mb,(size_t)main_obj.mapsize,&main_phnum);
  lib_ehdr_ok=sc_ehdr_phnum(lb,(size_t)lib.mapsize,&lib_phnum);
  main_dyn_ok=!sc_parse_dyn(mb,(size_t)main_obj.mapsize,main_obj.mapbase,&dm);
  lib_dyn_ok=!sc_parse_dyn(lb,(size_t)lib.mapsize,lib.mapbase,&dl);
  /* PLT-stub candidates + callers, scanned ONCE for both stages. On
   * overflow the candidate set is ambiguous by definition (fail loud). */
  int st=0;
  if(!stub_trunc)st=sc_scan_stubs(mb,(size_t)main_obj.mapsize,lb,(size_t)lib.mapsize,
                                  lib.mapbase,stubs,SC_MAX_STUBS,&stub_count,&stub_trunc);
  if(st||stub_trunc){reason="stub_ambiguous";status=3;stub_count=SC_MAX_STUBS;st=1;}
  else{
   uint64_t bound=(uint64_t)main_obj.mapsize;
   for(int k=0;k<stub_count;k++)
    if(stubs[k].stub_off<bound)bound=stubs[k].stub_off;
   sc_scan_calls(mb,bound,(size_t)main_obj.mapsize,stubs,stub_count);
  }
  /* Stage A, variant nid_reloc (brief primary; expected dead on 13.00):
   * the JUMP_SLOT named by the NID, found in the MAIN map's relocations.
   * Ambiguity here falls through to the later stages (it is recorded in the
   * sites, never force-fit); only a UNIQUE result may win. */
  if(!st&&nid_main_found&&main_dyn_ok){
   uint64_t g=0;unsigned sc_sites=0,sc_trunc=0;
   int r=sc_got_by_name(mb,(size_t)main_obj.mapsize,&dm,NID,main_obj.mapbase,&g,
                        sites+site_count,&sc_sites,&sc_trunc);
   site_count+=sc_sites;site_trunc=site_trunc||sc_trunc;
   if(r==1&&g+8<=(uint64_t)main_obj.mapsize){
    uint64_t got_value;memcpy(&got_value,mb+g,8);
    if(got_value>=lib.mapbase&&got_value-lib.mapbase+20<=(uint64_t)lib.mapsize&&
       !memcmp(lb+(size_t)(got_value-lib.mapbase),FWS_PROLOGUE16,FWS_PROLOGUE_LEN)){
     sc_stub win;
     if(sc_import_for_got(stubs,stub_count,g,&win)==1){
      strategy="nid_reloc";got_off=g;import_off=win.stub_off;
      wrapper=win.wrapper;wrapper_stack=win.wrapper_stack;
     }
    }
   }
  }
  /* Stage A, variant nid_export: libkernel_sys exports the NID; its
   * st_value is the wrapper, the GOT slot is the unique main-map qword. */
  if(!st&&!strategy&&nid_lib_found&&lib_dyn_ok){
   sc_symhit hits[8];int trunc=0;
   int n=sc_find_syms_by_name(lb,(size_t)lib.mapsize,&dl,NID,hits,8,&trunc);
   if(n==1&&!trunc){
    uint64_t w=hits[0].st_value;
    if(w+20<=(uint64_t)lib.mapsize&&
       !memcmp(lb+(size_t)w,FWS_PROLOGUE16,FWS_PROLOGUE_LEN)){
     uint32_t ws;memcpy(&ws,lb+(size_t)w+16,4);
     uint64_t q[16];int qtrunc=0;
     int qn=sc_find_qword(mb,(size_t)main_obj.mapsize,lib.mapbase+w,q,16,&qtrunc);
     if(qn==1&&!qtrunc&&q[0]+8<=(uint64_t)main_obj.mapsize){
      sc_stub win;
      if(sc_import_for_got(stubs,stub_count,q[0],&win)==1){
       strategy="nid_export";got_off=q[0];import_off=win.stub_off;
       wrapper=w;wrapper_stack=ws;
      }
     }
    }
   }
  }
  /* Stage B (documented fallback, expected path): the unique PLT stub into
   * libkernel_sys with the profile prologue that has exactly one e8 caller. */
  if(!st&&!strategy){
   /* The profile's call is the UNIQUE e8 rel32 targeting import_: a stub
    * with 0 callers is not BeginAppMount2 (it is called), a stub with >1
    * callers has no unique call. Exactly one stub may carry exactly one
    * caller. */
   sc_stub*win=NULL;int winners=0,callable=0;
   for(int k=0;k<stub_count;k++){
    if(!stubs[k].calls)continue;
    callable++;
    if(stubs[k].calls>1)continue;
    if(!winners)win=&stubs[k];
    winners++;
   }
   if(!winners){
    reason=callable?"call_ambiguous":"no_stub_with_call";
    status=3;
    if(callable)for(int k=0;k<stub_count&&site_count<16;k++){
     if(stubs[k].calls<=1)continue;
     for(unsigned c=0;c<stubs[k].calls&&c<4;c++)
      if(site_count<16)sites[site_count++]=stubs[k].call_off[c];
     if(stubs[k].calls>4)site_trunc=1;
    }
   }else if(winners>1){
    reason="stub_ambiguous";status=3;
    for(int k=0;k<stub_count;k++){
     if(stubs[k].calls!=1)continue;
     if(site_count<16)sites[site_count++]=stubs[k].stub_off;
     else{site_trunc=1;break;}
    }
   }else{
    strategy="plt_stub";got_off=win->got_off;import_off=win->stub_off;
    wrapper=win->wrapper;wrapper_stack=win->wrapper_stack;
   }
  }
 }
 if(!reason&&strategy){
  /* the winning stub's single caller (Stage A went through
   * sc_import_for_got, which requires calls==1; Stage B checked it above) */
  call_off=0;
  for(int k=0;k<stub_count;k++)
   if((uint64_t)stubs[k].stub_off==import_off&&stubs[k].calls==1)
    call_off=stubs[k].call_off[0];
  if(!call_off)reason="call_not_found";
 }
 if(!reason&&strategy){
  memcpy(call_bytes,mb+call_off,5);
  memcpy(stub_bytes,mb+import_off,6);
  function_off=sc_nearest_prologue(mb,call_off,&prologue_total);
  if(!function_off)reason="function_prologue_not_found";
  else memcpy(&function_stack,mb+function_off+16,4);
 }
 if(!reason&&strategy){
  padding_ok=!sc_derive_padding(pid,main_obj.mapbase,main_obj.mapsize,main_obj.textsize,
                                mb,call_off,&padding,&padding_source,&rx_end,&rx_walked);
  rx_printed=rx_walked;
  if(!padding_ok)reason="padding_derivation_failed";
 }
 if(!reason&&strategy&&padding_ok){
  pend=fws_end(padding);pcave=fws_cave(padding,pend);
  reserve_ok=fws_reserve_bounds_ok(padding,pcave,pend);
  P.fw=fw;P.function=(uint32_t)function_off;P.call=(uint32_t)call_off;
  P.import_=(uint32_t)import_off;P.got=(uint32_t)got_off;P.wrapper=(uint32_t)wrapper;
  P.padding=padding;P.cave=pcave;P.end=pend;
  memcpy(P.call_bytes,call_bytes,5);memcpy(P.stub,stub_bytes,6);
  P.function_stack=function_stack;P.wrapper_stack=wrapper_stack;
  have_profile=1;
  sc_read_ctx ctx={pid};
  dryrun_pass=fws_bm_valid_dry_run(main_obj.mapbase,lib.mapbase,&P,sc_read_mem,&ctx,&failing_check);
  if(!reserve_ok)reason="reserve_bounds_failed";
  else if(!dryrun_pass)reason="bm_valid_dry_run_failed";
  if(reason)status=1;else status=0;
 }else if(!reason&&(!strategy||!padding_ok)){
  reason=strategy?"padding_derivation_failed":"nid_and_stub_resolution_failed";
  status=1;
 }
 /* session record: resolution, raw dynlib fields, NID lookups, strategy and
  * any ambiguity sites (offsets only - never raw bytes). */
 begin("session");printf(",\"mode\":\"shellcore\",\"firmware_raw\":");
 if(!fw_rc&&fw_size==4)printf("\"0x%08x\"",fw);else fputs("null",stdout);
 printf(",\"firmware_rc\":%d,\"firmware_errno\":%d,\"shellcore_pid\":",fw_rc,fw_error);
 if(pid>=0)printf("%d",pid);else fputs("null",stdout);
 printf(",\"shellcore_obj_rc\":%d,\"mapbase\":",obj_rc);
 hexopt(resolved,main_obj.mapbase);
 fputs(",\"mapsize\":",stdout);hexopt(resolved,main_obj.mapsize);
 fputs(",\"textsize_raw\":",stdout);hexopt(resolved,main_obj.textsize);
 fputs(",\"eh_frame_hdr_raw\":",stdout);hexopt(resolved,main_obj.eh_frame_hdr);
 printf(",\"libkernel_sys_handle_rc\":%d,\"libkernel_sys_handle\":",handle_rc);
 hexopt(!handle_rc,handle);
 printf(",\"libkernel_sys_obj_rc\":%d,\"libkernel_sys_base\":",lib_rc);
 hexopt(lib_resolved,lib.mapbase);
 fputs(",\"libkernel_sys_mapsize\":",stdout);hexopt(lib_resolved,lib.mapsize);
 fputs(",\"nid\":",stdout);string(NID);
 printf(",\"nid_main_present\":%s,\"nid_main_offset\":",nid_main_found?"true":"false");
 hexopt(nid_main_found,nid_main_off);
 printf(",\"nid_lib_present\":%s,\"nid_lib_offset\":",nid_lib_found?"true":"false");
 hexopt(nid_lib_found,nid_lib_off);
 printf(",\"main_dyn_parsed\":%s,\"lib_dyn_parsed\":%s,\"main_ehdr_ok\":%s,\"main_phnum\":%u,"
        "\"lib_ehdr_ok\":%s,\"lib_phnum\":%u,\"stub_candidates\":%d,"
        "\"stub_truncated\":%s,\"prologues_before_call\":%llu,\"strategy\":",
        main_dyn_ok?"true":"false",lib_dyn_ok?"true":"false",
        main_ehdr_ok?"true":"false",main_phnum,
        lib_ehdr_ok?"true":"false",lib_phnum,stub_count,
        stub_trunc?"true":"false",(unsigned long long)prologue_total);
 if(strategy)string(strategy);else fputs("null",stdout);
 /* Per-candidate derivation details (offsets and byte-derived values only):
  * on ambiguity every candidate is a self-consistent tuple - each would pass
  * the bm_valid dry run by construction - so the choice between them cannot
  * be made from structure alone; the record reports instead of forcing. */
 fputs(",\"candidates\":[",stdout);
 if(mb){
  unsigned printed=0;
  for(int k=0;k<stub_count;k++){
   uint32_t co,cf;uint32_t fs=0;uint64_t gap,ptotal;
   if(!stubs[k].calls)continue;
   if(printed>=16){site_trunc=1;break;}
   co=stubs[k].call_off[0];
   cf=(uint32_t)sc_nearest_prologue(mb,co,&ptotal);
   if(cf)memcpy(&fs,mb+cf+16,4);
   gap=cf?co-cf:0;
   printf("%s{\"stub\":\"0x%lx\",\"got\":\"0x%lx\",\"wrapper\":\"0x%lx\","
          "\"wrapper_stack\":\"0x%lx\",\"call\":\"0x%lx\",\"caller_function\":\"0x%lx\","
          "\"function_stack\":\"0x%lx\",\"function_gap\":\"0x%lx\",\"calls\":%u}",
          printed?",":"",
          (unsigned long)stubs[k].stub_off,
          (unsigned long)stubs[k].got_off,(unsigned long)stubs[k].wrapper,
          (unsigned long)stubs[k].wrapper_stack,(unsigned long)co,
          (unsigned long)cf,(unsigned long)fs,(unsigned long)gap,stubs[k].calls);
   printed++;
  }
 }
 fputs("]",stdout);
 printf(",\"padding_source\":");
 if(padding_source)string(padding_source);else fputs("null",stdout);
 printf(",\"rx_end\":");
 if(rx_printed)printf("\"0x%lx\"",(unsigned long)rx_end);else fputs("null",stdout);
 fputs(",\"sites\":[",stdout);
 for(unsigned i=0;i<site_count;i++)printf("%s\"0x%lx\"",i?",":"",(unsigned long)sites[i]);
 printf("],\"sites_truncated\":%s,\"reason\":",site_trunc?"true":"false");
 if(reason)string(reason);else fputs("null",stdout);
 printf(",\"ok\":%s}\n",status?"false":"true");
 if(have_profile)fws_print_profile(stdout,&P,padding_source,reserve_ok,dryrun_pass,failing_check);
 begin("end");printf(",\"mode\":\"shellcore\",\"ok\":%s,\"reason\":",status?"false":"true");
 if(reason)string(reason);else fputs("null",stdout);
 fputs("}\n",stdout);
 free(mb);free(lb);
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
 if(mode&&!strcmp(mode,"--shellcore"))return shellcore_mode();
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
  if(!strcmp(flag,"shellcore"))return shellcore_mode();
 }
 return probe_mode();
}
