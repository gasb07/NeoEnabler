/* GPL-3.0-only. JSONL record emitters for the firmware offset scanner.
 * printf-style into a FILE*, host-compilable: the host regression test calls
 * the same functions the console payload uses, so record shape (single-brace
 * objects, valid JSON) is covered without a console.
 *
 * Include ../src/neo_budget_data.h first (mbd_layout/mbd_snapshot come from
 * there; host tests define NEO_DATA_BUDGET_TEST and mock MBD_KREAD/MBD_KWRITE
 * before including it, budget_data_test.c pattern). This header itself pulls
 * in only <stdio.h>/<stdint.h> plus that header.
 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include "../src/neo_budget_data.h"
#include "fw_offset_scan.h" /* fws_profile (bm_profile shape without the process-control header) */

#define FWS_FORMAT "neo-fwscan-v1"

static inline void fws_hexq(FILE*f,uint64_t v){
 fprintf(f,"\"0x%016lx\"",(unsigned long)v);
}
static inline void fws_print_layout(FILE*f,const mbd_layout*L){
 fprintf(f,"{\"fw\":\"0x%08x\",\"backing_base\":\"0x%lx\",\"container_ptr\":\"0x%lx\","
            "\"ext_global\":\"0x%lx\",\"references\":\"0x%lx\",\"profile\":\"0x%lx\","
            "\"maximum\":\"0x%lx\"}",L->fw,(unsigned long)L->backing_base,
            (unsigned long)L->container_ptr,(unsigned long)L->ext_global,
            (unsigned long)L->references,(unsigned long)L->profile,
            (unsigned long)L->maximum);
}
/* Snapshot FIELD VALUES only — no braces: the caller owns the object braces
 * (an inner `{` here produced `"snapshot":{"read_rc":0,{...}}`, a keyless
 * nested object that fails json.loads). read_rc is printed by the caller. */
static inline void fws_print_snapshot(FILE*f,const mbd_snapshot*s){
 fputs("\"extension\":",f);fws_hexq(f,s->extension);
 fputs(",\"container\":",f);fws_hexq(f,s->container);
 fprintf(f,",\"references\":\"0x%lx\",\"profile\":\"0x%lx\"",
            (unsigned long)s->references,(unsigned long)s->profile);
 fputs(",\"maximum\":",f);fws_hexq(f,s->maximum);
 fputs(",\"backing_base\":",f);fws_hexq(f,s->backing_base);
 fputs(",\"available\":",f);fws_hexq(f,s->available);
 fputs(",\"field38\":",f);fws_hexq(f,s->field38);
 fputs(",\"reserved\":",f);fws_hexq(f,s->reserved);
 fprintf(f,",\"app_id\":\"0x%lx\",\"category\":\"0x%lx\",\"mode\":\"0x%lx\",\"flags\":\"0x%lx\"",
            (unsigned long)s->app_id,(unsigned long)s->category,
            (unsigned long)s->mode,(unsigned long)s->flags);
}
/* One complete candidate record per line; exactly one brace level around the
 * snapshot object in both the success and the failed-read path. */
static inline void fws_print_candidate(FILE*f,const char*state,uint32_t backing_off,
      const mbd_layout*L,int read_rc,int read_errno,const mbd_snapshot*s,
      int eligible,int idle_ok){
 fprintf(f,"{\"format\":\"" FWS_FORMAT "\",\"record\":\"candidate\",\"mode\":\"data\",\"state\":\"%s\"",state);
 fprintf(f,",\"backing_off\":\"0x%lx\",\"layout\":",(unsigned long)backing_off);
 fws_print_layout(f,L);
 fputs(",\"snapshot\":{",f);
 if(read_rc)fprintf(f,"\"read_rc\":%d,\"errno\":%d}",read_rc,read_errno);
 else{fputs("\"read_rc\":0,",f);fws_print_snapshot(f,s);fputc('}',f);}
 fprintf(f,",\"eligible\":%s,\"idle_ok\":%s}\n",eligible?"true":"false",
            idle_ok?"true":"false");
}

/* ShellCore profile record (mode shellcore): exactly one brace level around
 * the "profile" object and around "bm_valid_dry_run", both opened and closed
 * here. padding_source names the derivation method (textsize | rx_tail_scan |
 * zero_run_scan; null when no padding was derived); failing_check is null on
 * a passing dry run, otherwise the first failed bm_valid check. */
static inline void fws_print_bytes(FILE*f,const unsigned char*b,size_t n){
 for(size_t i=0;i<n;i++)fprintf(f,"%02x",b[i]);
}
static inline void fws_print_profile(FILE*f,const fws_profile*p,const char*padding_source,
                                     int reserve_ok,int dryrun_pass,const char*failing_check){
 fprintf(f,"{\"format\":\"" FWS_FORMAT "\",\"record\":\"profile\",\"mode\":\"shellcore\",\"fw\":\"0x%08x\","
         "\"profile\":{\"function\":\"0x%lx\",\"call\":\"0x%lx\",\"import_\":\"0x%lx\",\"got\":\"0x%lx\","
         "\"wrapper\":\"0x%lx\",\"padding\":\"0x%lx\",\"cave\":\"0x%lx\",\"end\":\"0x%lx\",\"call_bytes_hex\":\"",
   p->fw,(unsigned long)p->function,(unsigned long)p->call,(unsigned long)p->import_,
   (unsigned long)p->got,(unsigned long)p->wrapper,(unsigned long)p->padding,
   (unsigned long)p->cave,(unsigned long)p->end);
 fws_print_bytes(f,p->call_bytes,5);
 fputs("\",\"stub_hex\":\"",f);
 fws_print_bytes(f,p->stub,6);
 fprintf(f,"\",\"function_stack\":\"0x%lx\",\"wrapper_stack\":\"0x%lx\"},",
   (unsigned long)p->function_stack,(unsigned long)p->wrapper_stack);
 fputs("\"padding_source\":",f);
 if(padding_source)fprintf(f,"\"%s\"",padding_source);else fputs("null",f);
 fprintf(f,",\"reserve_bounds_ok\":%s,\"bm_valid_dry_run\":{\"pass\":%s,\"failing_check\":",
   reserve_ok?"true":"false",dryrun_pass?"true":"false");
 if(failing_check)fprintf(f,"\"%s\"",failing_check);else fputs("null",f);
 fputs("}}\n",f);
}

/* One e8 site of the blessed import_ (Ruling 2 evidence): site offset,
 * enclosing function (nearest 20-byte prologue), its byte-derived stack, the
 * gap against the 0xe51 invariant, where the real-prologue filter found the
 * entry (null when the site does not survive) and the verdict. Array
 * element: the caller owns the surrounding [ ] and commas. */
static inline void fws_print_site(FILE*f,uint64_t site,uint64_t function,
                                  uint32_t function_stack,uint64_t function_gap,
                                  uint64_t real_prologue_at,int survives){
 fprintf(f,"{\"site\":\"0x%lx\",\"function\":\"0x%lx\",\"function_stack\":\"0x%lx\","
           "\"function_gap\":\"0x%lx\",\"real_prologue_at\":",
   (unsigned long)site,(unsigned long)function,(unsigned long)function_stack,
   (unsigned long)function_gap);
 if(real_prologue_at)fprintf(f,"\"0x%lx\"",(unsigned long)real_prologue_at);
 else fputs("null",f);
 fprintf(f,",\"prologue_window\":\"0x%lx\",\"survives\":%s}",
   (unsigned long)FWS_PROLOGUE_WINDOW,survives?"true":"false");
}
