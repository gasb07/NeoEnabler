/* GPL-3.0-only. Host test of the scanner derivation helpers.
 * neo_budget_data.h is pulled in under test defines with MBD_KREAD mocked
 * (budget_data_test.c pattern); the mock stays macro-only here because the
 * snapshot reads are exercised by the scoring test added with the --data mode.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define NEO_DATA_BUDGET_TEST
#define MBD_DATA() 0xffffffffa0000000ULL
#define MBD_KREAD(a,p,n)  ((void)(a),(void)(p),(void)(n),0)
#define MBD_KWRITE(p,a,n) ((void)(p),(void)(a),(void)(n),0)
#include "../src/neo_budget_data.h"
#include "../diagnostics/fw_offset_scan.h"
int main(void){
 /* delta reconstruction reproduces all six real layouts from backing alone */
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
 /* candidate selection: dedup + cap */
 uint32_t out[4]; uint64_t hits[] = {0x100000000, 0x100000000, 0x200000000};
 assert(fws_backing_candidates(hits, 3, out, 4) == 1 && out[0] == 0);
 puts("fw offset scan: layout deltas, cave cap, bounds and candidates passed");
}
