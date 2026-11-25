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
