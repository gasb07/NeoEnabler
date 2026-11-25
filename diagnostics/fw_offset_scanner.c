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
 * a plain deploy must produce the probe output. Later phases add
 * "--data <live|idle>" and "--shellcore" to the same dispatch.
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
int main(int argc,char**argv){
 setvbuf(stdout,NULL,_IONBF,0);
 const char*mode=argc>1?argv[1]:"--probe";
 if(!strcmp(mode,"--probe"))return probe_mode();
 begin("error");printf(",\"mode\":");string(mode);
 fputs(",\"reason\":\"unknown_mode\"}\n",stdout);
 return 1;
}
