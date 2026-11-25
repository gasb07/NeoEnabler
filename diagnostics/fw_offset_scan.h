/* GPL-3.0-only. Pure derivation helpers shared by every firmware offset
 * scanner mode. No I/O, no process control: the console payload and the host
 * tests compile the same code.
 *
 * Include ../src/neo_budget_data.h first: mbd_layout comes from there (host
 * tests: with NEO_DATA_BUDGET_TEST and MBD_KREAD/MBD_KWRITE mocked; console
 * payload: with MBD_KREAD = kernel_copyout). This header itself pulls in only
 * <stdint.h>/<stddef.h>.
 *
 * bm_profile field semantics (src/budget_mount_hook.h) are documented here by
 * contract only; that header is NOT included because it drags in process
 * control: padding = ShellCore main text size, cave = reservation start in the
 * text tail, end = 16 KiB-aligned text end. fws_reserve_bounds_ok() mirrors the
 * bm_reserve() entry checks, so a derived tuple refused here would also be
 * refused there.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Spec delta table (docs/superpowers/specs/2026-11-24-fw1300-8gb-scanner-design.md):
 * firmware-invariant offsets from backing_base, verified on all six stored
 * layouts (9.00, 10.01, 12.20, 12.70, 13.20, 13.60) and confirmed field by
 * field on 13.00 by the scanner before any write. */
#define FWS_DELTA_CONTAINER_PTR 0x28u
#define FWS_DELTA_EXT_GLOBAL    0x6100u
#define FWS_DELTA_REFERENCES    0x6158u
#define FWS_DELTA_PROFILE       0x7448u
#define FWS_DELTA_MAXIMUM       0x7460u

/* Rebuild a full mbd_layout from the backing base alone; backing_base is the
 * delta origin itself and fw is carried through unchanged. */
static inline mbd_layout fws_layout_from_backing(uint32_t fw, uint32_t backing_off){
 mbd_layout l;
 l.fw=fw;
 l.backing_base=backing_off;
 l.container_ptr=backing_off+FWS_DELTA_CONTAINER_PTR;
 l.ext_global=backing_off+FWS_DELTA_EXT_GLOBAL;
 l.references=backing_off+FWS_DELTA_REFERENCES;
 l.profile=backing_off+FWS_DELTA_PROFILE;
 l.maximum=backing_off+FWS_DELTA_MAXIMUM;
 return l;
}

/* Spec Background: end is the 16 KiB alignment of padding. */
static inline uint32_t fws_end(uint32_t padding){
 return (padding+0x3fffU)&~0x3fffU;
}

/* Spec Background: cave = max(align_up(padding, 1024), end-8192). The 8 KiB
 * cap beats a plain 1024-align: 13.20 padding 0x19619ec aligns to 0x1961c00,
 * which sits 9216 from end 0x1964000 and would be refused by bm_reserve();
 * the profile's actual cave 0x1962000 satisfies the cap. */
static inline uint32_t fws_cave(uint32_t padding, uint32_t end){
 uint32_t align=(padding+1023U)&~1023U;
 uint32_t capped=end-8192U;
 return align>capped?align:capped;
}

/* Mirrors the bm_reserve() entry checks (src/budget_mount_hook.h:151-152):
 * cave 1024-aligned, cave >= padding, end == fws_end(padding),
 * end-cave <= 8192 (an unsigned end<cave wraps huge and fails the last test). */
static inline int fws_reserve_bounds_ok(uint32_t padding, uint32_t cave, uint32_t end){
 return !(cave&1023U)&&cave>=padding&&end==fws_end(padding)&&end-cave<=8192U;
}

/* ShellCore BeginAppMount2 profile: the same fields as bm_profile
 * (src/budget_mount_hook.h:12-18) without including that header (it drags in
 * process control). Discovery populates it; fws_bm_valid_dry_run() re-checks
 * the bm_valid() byte semantics read-only against it. */
typedef struct {
 uint32_t fw, function, call, import_, got, wrapper, padding, cave, end;
 unsigned char call_bytes[5], stub[6];
 uint32_t function_stack, wrapper_stack;
} fws_profile;

/* The shared 16-byte prologue prefix (push rbp; mov rbp,rsp; push r15; push
 * r14; push r13; push r12; push rbx; sub rsp,imm32). The imm32 at +16 (frame
 * size) is DATA derived from the bytes found, never hardcoded. */
#define FWS_PROLOGUE_LEN 16
static const unsigned char FWS_PROLOGUE16[FWS_PROLOGUE_LEN]={
 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec};

/* Read-only twin of bm_valid() (src/budget_mount_hook.h:109-123): the same
 * checks in the same order - 20-byte ShellCore prologue (FWS_PROLOGUE16 with
 * the profile function_stack at +16) at base+function, call_bytes[5] at
 * base+call, stub[6] at base+import_, GOT qword at base+got == lib+wrapper,
 * wrapper prologue (wrapper_stack at +16) at the GOT target - with the memory
 * read injected so host tests feed mock bytes and the console payload reads
 * through kernel_proc_copyout. Returns 1 on pass; on failure 0 and
 * *failing_check names the check: "<name>" on a byte mismatch,
 * "<name>_unreadable" when the read fails. Check names: prologue, call_bytes,
 * stub, got_target, wrapper. */
typedef int (*fws_read_mem)(void*,uint64_t,void*,size_t);
static inline int fws_bm_valid_dry_run(uint64_t base,uint64_t lib,const fws_profile*p,
                                       fws_read_mem read_mem,void*ctx,
                                       const char**failing_check){
 unsigned char want_pro[20],want_wrap[20],actual[24];
 uint64_t got_value=0;
 const char*fail=NULL;
 memcpy(want_pro,FWS_PROLOGUE16,FWS_PROLOGUE_LEN);
 memcpy(want_pro+16,&p->function_stack,4);
 memcpy(want_wrap,FWS_PROLOGUE16,FWS_PROLOGUE_LEN);
 memcpy(want_wrap+16,&p->wrapper_stack,4);
 if(read_mem(ctx,base+p->function,actual,sizeof want_pro))fail="prologue_unreadable";
 else if(memcmp(actual,want_pro,sizeof want_pro))fail="prologue";
 if(!fail){
  if(read_mem(ctx,base+p->call,actual,5))fail="call_unreadable";
  else if(memcmp(actual,p->call_bytes,5))fail="call_bytes";
 }
 if(!fail){
  if(read_mem(ctx,base+p->import_,actual,6))fail="stub_unreadable";
  else if(memcmp(actual,p->stub,6))fail="stub";
 }
 if(!fail){
  if(read_mem(ctx,base+p->got,&got_value,8))fail="got_unreadable";
  else if(got_value!=lib+p->wrapper)fail="got_target";
 }
 if(!fail){
  if(read_mem(ctx,got_value,actual,sizeof want_wrap))fail="wrapper_unreadable";
  else if(memcmp(actual,want_wrap,sizeof want_wrap))fail="wrapper";
 }
 *failing_check=fail;
 return !fail;
}

/* Index-vs-offset contract: out[] receives indices into hits[], never DATA
 * offsets. The caller translates index -> DATA offset as scan base + index*8,
 * where the scan base is the DATA offset of hits[0]. Only the value
 * 0x100000000 (the backing base mbd_eligible() requires) is collected, and
 * EVERY occurrence is reported: a later same-value hit is a real candidate
 * too and must be validated, not deduped away. Returns the number of
 * candidates copied (0..cap); when more than cap matches exist, out[] holds
 * the first cap indices and the return value is -cap (truncation signal;
 * cap>=1 so it cannot be confused with "0 matches"). Callers must treat a
 * negative return as a failed scan, never a clean verdict. */
static inline int fws_backing_candidates(const uint64_t *hits, size_t n, uint32_t *out, size_t cap){
 size_t copied=0;
 if(!hits||!out||!cap)return 0;
 for(size_t i=0;i<n;i++){
  if(hits[i]!=0x100000000ULL)continue;
  if(copied==cap)return -(int)cap; /* out full, more matches exist: truncated */
  out[copied++]=(uint32_t)i;
 }
 return (int)copied;
}
