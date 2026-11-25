/* GPL-3.0-only. Host test of the scanner derivation helpers and the --data
 * scoring path. neo_budget_data.h is pulled in under test defines with
 * MBD_KREAD mocked as fake DATA memory (budget_data_test.c read_data FIELD
 * pattern); MBD_KWRITE stays macro-only because the scoring test is read-only
 * (the write path is exercised by budget_data_test.c). The scoring test feeds
 * a layout reconstructed by fws_layout_from_backing() into the production
 * mbd_read_snapshot()/mbd_eligible() and checks the live and idle signatures.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NEO_DATA_BUDGET_TEST
#define MBD_DATA() 0xffffffffa0000000ULL
static int read_data(uint64_t,void*,size_t);
#define MBD_KREAD(a,p,n) read_data(a,p,n)
#define MBD_KWRITE(p,a,n) ((void)(p),(void)(a),(void)(n),0)
#include "../src/neo_budget_data.h"
#include "../diagnostics/fw_offset_scan.h"
#include "../diagnostics/fw_record.h"
static const mbd_layout *layout;
static mbd_snapshot live;
static uint64_t descriptor=0xffff800000002000ULL;
static int fail_read;
/* Minimal JSON sanity: balanced braces, no keyless nested object (the
 * `"snapshot":{"read_rc":0,{...}}` bug), and a "snapshot" object that is a
 * single brace level containing key read_rc. Returns 1 when the line is a
 * plausible candidate record. */
static int candidate_json_ok(const char*line){
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
 if(depth)return 0;
 if(nested_after_comma)return 0;
 const char*sn=strstr(line,"\"snapshot\":{");
 if(!sn)return 0;
 sn+=strlen("\"snapshot\":{");
 int sdepth=1;
 for(const char*p=sn;*p&&sdepth;p++){
  if(*p=='{')sdepth++;
  else if(*p=='}')sdepth--;
  else if(*p=='"'){p++;while(*p&&*p!='"'){if(*p=='\\')p++;if(*p)p++;}if(!*p)return 0;}
 }
 if(sdepth)return 0;
 return strstr(sn,"\"read_rc\"")!=NULL;
}
static int read_data(uint64_t a,void *p,size_t n){
 if(fail_read)return -1;
 /* Any unexpected address, including TEXT, is a test failure. */
 #define FIELD(off,field) if(a==MBD_DATA()+layout->off){assert(n==sizeof live.field);memcpy(p,&live.field,n);return 0;}
 FIELD(ext_global,extension)
 FIELD(maximum,maximum)
 FIELD(container_ptr,container)
 FIELD(backing_base,backing_base)
 FIELD(references,references)
 FIELD(profile,profile)
 #undef FIELD
 if(a==live.container){
  assert(n==0x148);memset(p,0,n);
  #define PUT(off,field) memcpy((char*)p+off,&live.field,sizeof live.field)
  memcpy((char*)p+8,&descriptor,8);
  PUT(0x38,field38);PUT(0x40,reserved);PUT(0x80,available);
  PUT(0x118,category);PUT(0x11c,app_id);PUT(0x140,flags);
  #undef PUT
  return 0;
 }
 if(a==descriptor+0x58){assert(n==1);*(unsigned char*)p=(unsigned char)live.mode;return 0;}
 assert(!"unexpected read");return -1;
}
int main(void){
 /* delta reconstruction reproduces all seven real layouts from backing alone */
 assert(fws_layout_from_backing(0x13200000, 0x2ed9fb8).container_ptr == 0x2ed9fe0);
 assert(fws_layout_from_backing(0x13200000, 0x2ed9fb8).ext_global   == 0x2ee00b8);
 assert(fws_layout_from_backing(0x13200000, 0x2ed9fb8).references   == 0x2ee0110);
 assert(fws_layout_from_backing(0x13200000, 0x2ed9fb8).profile      == 0x2ee1400);
 assert(fws_layout_from_backing(0x13200000, 0x2ed9fb8).maximum      == 0x2ee1418);
 /* same for 9.00 (0x2d8dfb8), 10.01 (0x2d55fb8), 12.70 (0x2e81fb8), 13.60 (0x2eedfb8) */
 assert(fws_layout_from_backing(0x09000000, 0x2d8dfb8).container_ptr == 0x2d8dfe0);
 assert(fws_layout_from_backing(0x09000000, 0x2d8dfb8).ext_global   == 0x2d940b8);
 assert(fws_layout_from_backing(0x09000000, 0x2d8dfb8).references   == 0x2d94110);
 assert(fws_layout_from_backing(0x09000000, 0x2d8dfb8).profile      == 0x2d95400);
 assert(fws_layout_from_backing(0x09000000, 0x2d8dfb8).maximum      == 0x2d95418);
 assert(fws_layout_from_backing(0x10010000, 0x2d55fb8).container_ptr == 0x2d55fe0);
 assert(fws_layout_from_backing(0x10010000, 0x2d55fb8).ext_global   == 0x2d5c0b8);
 assert(fws_layout_from_backing(0x10010000, 0x2d55fb8).references   == 0x2d5c110);
 assert(fws_layout_from_backing(0x10010000, 0x2d55fb8).profile      == 0x2d5d400);
 assert(fws_layout_from_backing(0x10010000, 0x2d55fb8).maximum      == 0x2d5d418);
 assert(fws_layout_from_backing(0x12200000, 0x2e81fb8).container_ptr == 0x2e81fe0);
 assert(fws_layout_from_backing(0x12200000, 0x2e81fb8).ext_global   == 0x2e880b8);
 assert(fws_layout_from_backing(0x12200000, 0x2e81fb8).references   == 0x2e88110);
 assert(fws_layout_from_backing(0x12200000, 0x2e81fb8).profile      == 0x2e89400);
 assert(fws_layout_from_backing(0x12200000, 0x2e81fb8).maximum      == 0x2e89418);
 assert(fws_layout_from_backing(0x12700000, 0x2e81fb8).container_ptr == 0x2e81fe0);
 assert(fws_layout_from_backing(0x12700000, 0x2e81fb8).ext_global   == 0x2e880b8);
 assert(fws_layout_from_backing(0x12700000, 0x2e81fb8).references   == 0x2e88110);
 assert(fws_layout_from_backing(0x12700000, 0x2e81fb8).profile      == 0x2e89400);
 assert(fws_layout_from_backing(0x12700000, 0x2e81fb8).maximum      == 0x2e89418);
 /* 13.00 shares the 13.20 backing block (evidence/fw1300-data.json "layout") */
 assert(fws_layout_from_backing(0x13000000, 0x2ed9fb8).container_ptr == 0x2ed9fe0);
 assert(fws_layout_from_backing(0x13000000, 0x2ed9fb8).ext_global   == 0x2ee00b8);
 assert(fws_layout_from_backing(0x13000000, 0x2ed9fb8).references   == 0x2ee0110);
 assert(fws_layout_from_backing(0x13000000, 0x2ed9fb8).profile      == 0x2ee1400);
 assert(fws_layout_from_backing(0x13000000, 0x2ed9fb8).maximum      == 0x2ee1418);
 assert(fws_layout_from_backing(0x13600000, 0x2eedfb8).container_ptr == 0x2eedfe0);
 assert(fws_layout_from_backing(0x13600000, 0x2eedfb8).ext_global   == 0x2ef40b8);
 assert(fws_layout_from_backing(0x13600000, 0x2eedfb8).references   == 0x2ef4110);
 assert(fws_layout_from_backing(0x13600000, 0x2eedfb8).profile      == 0x2ef5400);
 assert(fws_layout_from_backing(0x13600000, 0x2eedfb8).maximum      == 0x2ef5418);
 /* the reconstruction matches every stored tuple field for field */
 for(size_t i=0;i<MBD_LAYOUT_COUNT;i++){
  mbd_layout got=fws_layout_from_backing(mbd_layouts[i].fw,mbd_layouts[i].backing_base);
  assert(got.fw==mbd_layouts[i].fw);
  assert(got.ext_global==mbd_layouts[i].ext_global);
  assert(got.container_ptr==mbd_layouts[i].container_ptr);
  assert(got.backing_base==mbd_layouts[i].backing_base);
  assert(got.references==mbd_layouts[i].references);
  assert(got.profile==mbd_layouts[i].profile);
  assert(got.maximum==mbd_layouts[i].maximum);
 }
 /* cave cap: plain align would give 0x1961c00 (9216 from end) -> must clamp */
 assert(fws_cave(0x19619ec, 0x1964000) == 0x1962000);   /* end-8192 */
 assert(fws_cave(0x19619ec, fws_end(0x19619ec)) == 0x1962000);
 assert(fws_end(0x19619ec) == 0x1964000);
 /* every existing profile tuple passes the bounds check */
 assert(fws_reserve_bounds_ok(0x19619ec, 0x1962000, 0x1964000)); /* 13.20 */
 assert(fws_reserve_bounds_ok(0x19626fc, 0x1963000, 0x1964000)); /* 13.60 */
 assert(fws_reserve_bounds_ok(0x17db60c, 0x17db800, 0x17dc000)); /* 10.01 */
 assert(!fws_reserve_bounds_ok(0x19619ec, 0x1961c00, 0x1964000)); /* the near-miss */
 /* candidate selection: every 0x100000000 hit is reported (spec: each hit
  * must be validated — a later same-value hit is a real candidate too);
  * cap bounds the output and truncation is signaled, never silent */
 uint32_t out[4]; uint64_t hits[] = {0x100000000, 0x100000000, 0x200000000};
 assert(fws_backing_candidates(hits, 3, out, 4) == 2 && out[0] == 0 && out[1] == 1);
 assert(fws_backing_candidates(hits, 3, out, 1) == -1);  /* cap=1, 2 matches: truncated */
 uint64_t none[] = {1, 2, 0x100000001};
 assert(fws_backing_candidates(none, 3, out, 4) == 0);   /* no matches */
 /* --data scoring: the 13.00 layout is derived from the backing hit alone through
  * the same chain the payload uses (independently of the stored mbd_layouts[] row,
  * which the loop above cross-checks):
  * chunk hits -> candidate index -> DATA offset (chunk base + index*8) ->
  * fws_layout_from_backing() -> production snapshot + eligibility. The chunk
  * base 0x2ed9fb0 with the hit at index 1 lands on the probe-observed qword
  * data_base+0x2ed9fb8. */
 mbd_layout l13;
 {
  uint64_t chunk[3]={0x4141414141414141ULL,0x100000000ULL,0x4242424242424242ULL};
  uint32_t idx=0;
  assert(fws_backing_candidates(chunk,3,&idx,1)==1&&idx==1);
  l13=fws_layout_from_backing(0x13000000u,0x2ed9fb0u+idx*8);
 }
 assert(l13.backing_base==0x2ed9fb8u&&l13.container_ptr==0x2ed9fe0u&&
        l13.ext_global==0x2ee00b8u&&l13.references==0x2ee0110u&&
        l13.profile==0x2ee1400u&&l13.maximum==0x2ee1418u);
 layout=&l13;
 memset(&live,0,sizeof live);live.container=0xffff800000001000ULL;
 live.app_id=1;live.category=0xf000;live.mode=2;live.references=live.profile=1;
 live.backing_base=0x100000000ULL;
 live.available=live.backing_base+MBD_EXPECTED_STOCK+MBD_DELTA;
 mbd_snapshot s;
 /* live signature: eligible (container heap ptr, category 0xf000,
  * references==profile==1, backing 0x100000000, available covers stock+delta) */
 assert(!mbd_read_snapshot(&l13,&s)&&mbd_eligible(&s));
 /* idle signature: container==0 -> snapshot reads fine but is not eligible */
 live.container=0;live.references=0;live.profile=0;
 assert(!mbd_read_snapshot(&l13,&s));
 assert(!mbd_eligible(&s));
 assert(s.extension==0&&s.maximum==0&&s.references==0&&s.container==0);
 /* candidate record shape: the emitted line must be valid single-brace JSON
  * with a "snapshot" object carrying read_rc (regression: the record used to
  * nest a keyless object, `"snapshot":{"read_rc":0,{...}}`, failing json.loads) */
 {
  FILE*tmp=tmpfile();assert(tmp);
  mbd_layout lc=fws_layout_from_backing(0x13000000u,0x2ed9fb8u);
  mbd_snapshot sc={0};
  sc.container=0xffff800000001000ULL;sc.app_id=1;sc.category=0xf000;
  sc.mode=2;sc.references=sc.profile=1;sc.backing_base=0x100000000ULL;
  sc.available=sc.backing_base+MBD_EXPECTED_STOCK+MBD_DELTA;
  fws_print_candidate(tmp,"live",0x2ed9fb8u,&lc,0,0,&sc,1,1);
  fws_print_candidate(tmp,"live",0x2ed9fb8u,&lc,-1,5,NULL,0,0);
  rewind(tmp);
  char line[1024];
  assert(fgets(line,sizeof line,tmp));
  assert(candidate_json_ok(line));
  assert(strstr(line,"\"eligible\":true"));
  assert(fgets(line,sizeof line,tmp));
  assert(candidate_json_ok(line)); /* failed-read record stays single-brace too */
  assert(strstr(line,"\"read_rc\":-1"));
  fclose(tmp);
 }
 puts("fw offset scan: layout deltas, cave cap, bounds, candidates and DATA scoring passed");
}
