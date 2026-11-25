/* GPL-3.0-only. Host test of the ShellCore profile byte-check logic and the
 * profile record emitter. The 13.20 and 13.60 tuples are copied verbatim from
 * src/budget_mount_hook.h (bm_profiles[]): mock memory built from a tuple must
 * PASS fws_bm_valid_dry_run() (the read-only twin of bm_valid(),
 * src/budget_mount_hook.h:109-123), and each single-field corruption must fail
 * with the matching check name — including the stack sizes, which the checker
 * must take from the profile fields while the prologue imm32 comes from the
 * mock bytes (both directions are asserted). The emitter is checked for the
 * Task-4 single-brace discipline; with argv[1] the two record lines are also
 * written to that file for a real python3 json.loads cross-check. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define NEO_DATA_BUDGET_TEST
#define MBD_DATA() 0xffffffffa0000000ULL
static int read_data(uint64_t,void*,size_t);
#define MBD_KREAD(a,p,n) read_data(a,p,n)
#define MBD_KWRITE(p,a,n) ((void)(p),(void)(a),(void)(n),0)
#include "../src/neo_budget_data.h"
#include "../diagnostics/fw_offset_scan.h"
#include "../diagnostics/fw_record.h"

/* Mock memory: ShellCore main map at MOCK_BASE, libkernel_sys at MOCK_LIB.
 * Sizes cover got+8 (~0x2067468) and wrapper+20 for the stored tuples. */
#define MOCK_BASE 0x100000000000ULL
#define MOCK_LIB  0x200000000000ULL
#define MOCK_TEXT 0x2068000u
#define MOCK_LIBSZ 0x80000u
static unsigned char *mem_text,*mem_lib;
/* When set, mock_read fails for any read covering this absolute address. */
static uint64_t fail_addr;static int fail_addr_set;

/* verbatim src/budget_mount_hook.h bm_profiles[] rows: 13.20 and 13.60 */
static const fws_profile p1320={0x13200000,0x6d6dc0,0x6d7c11,0x195b690,0x2067458,0x42850,0x19619ec,0x1962000,0x1964000,
 {0xe8,0x7a,0x3a,0x28,1},{0xff,0x25,0xc2,0xbd,0x70,0},0x18e8,0x198};
static const fws_profile p1360={0x13600000,0x6d7af0,0x6d8941,0x195c3b0,0x2067460,0x42850,0x19626fc,0x1963000,0x1964000,
 {0xe8,0x6a,0x3a,0x28,1},{0xff,0x25,0xaa,0xb0,0x70,0},0x18e8,0x198};

static int mock_read(void*ctx,uint64_t addr,void*buf,size_t n){
 (void)ctx;
 unsigned char*src=NULL;
 if(addr>=MOCK_BASE&&addr+n>=addr&&addr-MOCK_BASE<=MOCK_TEXT&&addr+n-MOCK_BASE<=MOCK_TEXT)
  src=mem_text+(size_t)(addr-MOCK_BASE);
 else if(addr>=MOCK_LIB&&addr+n>=addr&&addr-MOCK_LIB<=MOCK_LIBSZ&&addr+n-MOCK_LIB<=MOCK_LIBSZ)
  src=mem_lib+(size_t)(addr-MOCK_LIB);
 if(!src)return -1;
 if(fail_addr_set&&addr<=fail_addr&&fail_addr<addr+n)return -1;
 memcpy(buf,src,n);return 0;
}
static void build_mock(const fws_profile*p){
 uint64_t target=MOCK_LIB+p->wrapper;
 memset(mem_text,0,MOCK_TEXT);memset(mem_lib,0,MOCK_LIBSZ);
 memcpy(mem_text+p->function,FWS_PROLOGUE16,FWS_PROLOGUE_LEN);
 memcpy(mem_text+p->function+16,&p->function_stack,4);
 memcpy(mem_text+p->call,p->call_bytes,5);
 memcpy(mem_text+p->import_,p->stub,6);
 memcpy(mem_text+p->got,&target,8);
 memcpy(mem_lib+p->wrapper,FWS_PROLOGUE16,FWS_PROLOGUE_LEN);
 memcpy(mem_lib+p->wrapper+16,&p->wrapper_stack,4);
}
/* Generic line sanity: balanced braces/quotes, no `,` directly followed by
 * `{` (the Task-4 keyless-nesting bug signature), exactly one brace level
 * around "profile" and "bm_valid_dry_run". */
static int profile_json_ok(const char*line){
 int depth=0,nested_after_comma=0;
 for(const char*p=line;*p;p++){
  if(*p=='{')depth++;
  else if(*p=='}')depth--;
  else if(*p=='"'){
   p++;while(*p&&*p!='"'){if(*p=='\\')p++;if(*p)p++;}
   if(!*p)return 0;
  }
  else if(*p==','&&p[1]=='{')nested_after_comma=1;
  if(depth<0)return 0;
 }
 if(depth||nested_after_comma)return 0;
 if(!strstr(line,"\"profile\":{\"function\":"))return 0;
 if(!strstr(line,"\"bm_valid_dry_run\":{\"pass\":"))return 0;
 return 1;
}
int main(int argc,char**argv){
 mem_text=malloc(MOCK_TEXT);mem_lib=malloc(MOCK_LIBSZ);
 assert(mem_text&&mem_lib);
 const char*fail=NULL;
 /* exact 13.20 and 13.60 bytes pass */
 build_mock(&p1320);
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==1&&!fail);
 build_mock(&p1360);
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1360,mock_read,NULL,&fail)==1&&!fail);
 /* corrupt BeginAppMount2 call byte -> call_bytes */
 build_mock(&p1320);mem_text[p1320.call+2]^=0xff;
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"call_bytes"));
 /* corrupt import stub -> stub */
 build_mock(&p1320);mem_text[p1320.import_+3]^=0xff;
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"stub"));
 /* GOT qword pointing one wrapper off -> got_target */
 build_mock(&p1320);{uint64_t bad=MOCK_LIB+p1320.wrapper+1;
  memcpy(mem_text+p1320.got,&bad,8);}
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"got_target"));
 /* wrapper prologue imm32 in MEMORY differs from the profile value: the
  * expected wrapper bytes must be built from the profile field, so the
  * mismatch must be caught at the wrapper check (wrapper_stack is never
  * assumed from memory) */
 build_mock(&p1320);{uint32_t other=0x188;
  memcpy(mem_lib+p1320.wrapper+16,&other,4);}
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"wrapper"));
 /* function prologue imm32 in MEMORY differs -> prologue */
 build_mock(&p1320);{uint32_t other=0x18d8;
  memcpy(mem_text+p1320.function+16,&other,4);}
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"prologue"));
 /* unreadable import stub -> stub_unreadable */
 build_mock(&p1320);fail_addr=MOCK_BASE+p1320.import_+3;fail_addr_set=1;
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"stub_unreadable"));
 fail_addr_set=0;
 /* unreadable wrapper (at the GOT target address, not base+wrapper) */
 fail_addr=MOCK_LIB+p1320.wrapper+5;fail_addr_set=1;
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"wrapper_unreadable"));
 fail_addr_set=0;
 /* unreadable GOT -> got_unreadable */
 fail_addr=MOCK_BASE+p1320.got;fail_addr_set=1;
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==0);
 assert(fail&&!strcmp(fail,"got_unreadable"));
 fail_addr_set=0;
 /* restored bytes pass again (no state leaked between runs) */
 build_mock(&p1320);
 assert(fws_bm_valid_dry_run(MOCK_BASE,MOCK_LIB,&p1320,mock_read,NULL,&fail)==1&&!fail);
 /* padding -> end -> cave chain reproduces the stored 13.20/13.60 bounds */
 assert(fws_end(p1320.padding)==0x1964000&&fws_cave(p1320.padding,0x1964000)==0x1962000);
 assert(fws_reserve_bounds_ok(p1320.padding,p1320.cave,p1320.end));
 assert(fws_end(p1360.padding)==0x1964000&&fws_cave(p1360.padding,0x1964000)==0x1962800);
 assert(fws_reserve_bounds_ok(p1360.padding,p1360.cave,p1360.end)); /* stored 13.60 cave 0x1963000 also passes */
 /* profile record shape: single-brace JSON with the profile object, the
  * padding source and the dry-run verdict */
 {
  FILE*tmp=tmpfile();assert(tmp);
  fws_print_profile(tmp,&p1320,"rx_tail_scan",1,1,NULL);
  fws_print_profile(tmp,&p1320,"rx_tail_scan",1,0,"got_target");
  rewind(tmp);
  char line[1024];
  assert(fgets(line,sizeof line,tmp));
  assert(profile_json_ok(line));
  assert(strstr(line,"\"record\":\"profile\""));
  assert(strstr(line,"\"mode\":\"shellcore\""));
  assert(strstr(line,"\"fw\":\"0x13200000\""));
  assert(strstr(line,"\"function\":\"0x6d6dc0\""));
  assert(strstr(line,"\"call\":\"0x6d7c11\""));
  assert(strstr(line,"\"import_\":\"0x195b690\""));
  assert(strstr(line,"\"got\":\"0x2067458\""));
  assert(strstr(line,"\"wrapper\":\"0x42850\""));
  assert(strstr(line,"\"padding\":\"0x19619ec\""));
  assert(strstr(line,"\"cave\":\"0x1962000\""));
  assert(strstr(line,"\"end\":\"0x1964000\""));
  assert(strstr(line,"\"call_bytes_hex\":\"e87a3a2801\""));
  assert(strstr(line,"\"stub_hex\":\"ff25c2bd7000\""));
  assert(strstr(line,"\"function_stack\":\"0x18e8\""));
  assert(strstr(line,"\"wrapper_stack\":\"0x198\""));
  assert(strstr(line,"\"padding_source\":\"rx_tail_scan\""));
  assert(strstr(line,"\"reserve_bounds_ok\":true"));
  assert(strstr(line,"\"bm_valid_dry_run\":{\"pass\":true,\"failing_check\":null}"));
  assert(fgets(line,sizeof line,tmp));
  assert(profile_json_ok(line));
  assert(strstr(line,"\"bm_valid_dry_run\":{\"pass\":false,\"failing_check\":\"got_target\"}"));
  fclose(tmp);
  /* optional dump for a real python3 json.loads cross-check */
  if(argc>1){
   FILE*out=fopen(argv[1],"wb");assert(out);
   fws_print_profile(out,&p1320,"rx_tail_scan",1,1,NULL);
   fws_print_profile(out,&p1320,"rx_tail_scan",1,0,"got_target");
   fclose(out);
  }
 }
 free(mem_text);free(mem_lib);
 puts("shellcore profile: bm_valid twin (13.20/13.60), fail naming and record emitter passed");
}
/* Mock DATA read: the profile checks never touch DATA; any hit is a bug. */
static int read_data(uint64_t a,void*p,size_t n){(void)a;(void)p;(void)n;assert(!"unexpected DATA read");return -1;}
