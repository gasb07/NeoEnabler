/* Host-only: patches that need 8 GiB are turned off while 8 GiB is not available, other patches stay on, and the
   game filter works. Uses the Bloodborne XML of the repository in a fake ps-patch-system folder. Also checks that a
   collection keeps one resolution entry on per executable (AppElf), with an eboot.bin resolution under the others. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define ROOT "/tmp/neo-games-8gib-test"
#define PS_DIR ROOT "/xml_orbis"
#define PS_BACKUP_ROOT ROOT "/backup"
#define APPMETA_DIR ROOT "/appmeta"
#define APP_DIR ROOT "/app"
#define EXT_DRIVES ROOT "/ext0", ROOT "/ext1"
#define PATCH_DIR ROOT "/patch"
#define PS_NO_HTTP 1

static int logs;
static void log_line(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); logs++;
}
static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}
static char *ps_get(const char *url, size_t *len) { (void)url; (void)len; assert(0); return NULL; }

#include "../src/patch_sync.h"
#include "../src/games.h"

static void put(const char *path, const void *data, size_t n) { assert(!write_file(path, data, n)); }

/* A game in the fake console: its param.sfo and the repository patch file as ps-patch-system would have it. */
static void install_game(const char *tid, const char *title, const char *app_ver, const char *xml_dir, const char *xml_name) {
    char cmd[512], path[160];
    snprintf(cmd, sizeof cmd, "mkdir -p " APPMETA_DIR "/%s && cp %s/%s " PS_DIR "/%s.xml", tid, xml_dir, xml_name, tid);
    assert(!system(cmd));
    unsigned char sfo[256] = {0};
    memcpy(sfo, "\0PSF", 4);
    uint32_t keys = 20 + 3 * 16, data = keys + 32, count = 3;
    memcpy(sfo + 8, &keys, 4); memcpy(sfo + 12, &data, 4); memcpy(sfo + 16, &count, 4);
    const char *k[3] = {"APP_VER", "TITLE", "VERSION"}, *v[3] = {app_ver, title, "01.00"};
    uint16_t ko = 0; uint32_t dof = 0;
    for (int i = 0; i < 3; i++) {
        unsigned char *e = sfo + 20 + i * 16; uint32_t l = (uint32_t)strlen(v[i]) + 1;
        memcpy(e, &ko, 2); memcpy(e + 4, &l, 4); memcpy(e + 12, &dof, 4);
        memcpy(sfo + keys + ko, k[i], strlen(k[i]) + 1); memcpy(sfo + data + dof, v[i], l);
        ko += (uint16_t)(strlen(k[i]) + 1); dof += 32;
    }
    snprintf(path, sizeof path, APPMETA_DIR "/%s/param.sfo", tid);
    put(path, sfo, sizeof sfo);
}

static const gp_patch *pick(gp_patch *p, int n, int needs_8gib) {
    for (int i = 0; i < n; i++) if (!!p[i].needs_8gib == needs_8gib) return &p[i];
    return NULL;
}

int main(int argc, char **argv) {
    (void)game_needs; (void)patch_sync;   /* helpers of the includes that this test does not use */
    assert(argc == 2);
    assert(!system("rm -rf " ROOT " && mkdir -p " PS_DIR));
    install_game("CUSA00900", "Bloodborne", "01.09", argv[1], "Bloodborne_CUSA00900.xml");
    install_game("CUSA00190", "KILLZONE SHADOW FALL", "01.81", argv[1], "KillzoneShadowFall_CUSA00190.xml");

    int nb, nk; char name[128], ver[16];
    assert(!game_info("CUSA00900", name, sizeof name, ver, sizeof ver));
    gp_patch *bb = game_patches("CUSA00900", ver, &nb);
    assert(!game_info("CUSA00190", name, sizeof name, ver, sizeof ver));
    gp_patch *kz = game_patches("CUSA00190", ver, &nk);
    const gp_patch *big = pick(bb, nb, 1), *small = pick(kz, nk, 0);
    assert(big && small);
    char msg[2048];
    assert(!game_set_patch("CUSA00900", big->hash, 1, 1, msg, sizeof msg));
    assert(!game_set_patch("CUSA00190", small->hash, 1, 1, msg, sizeof msg));
    assert(game_set_patch("CUSA00900", big->hash, 1, 0, msg, sizeof msg) == -1);   /* refused when 8 GiB is unavailable */

    size_t sl; char *s = settings_read(&sl);
    assert(gp_setting(s, sl, big->hash) == 1 && gp_setting(s, sl, small->hash) == 1); free(s);

    assert(games_drop_8gib("CUSA00190", "test") == 0);                              /* another game: nothing */
    s = settings_read(&sl); assert(gp_setting(s, sl, big->hash) == 1); free(s);

    assert(games_drop_8gib(NULL, "test") == 1);                                     /* every game */
    s = settings_read(&sl);
    assert(gp_setting(s, sl, big->hash) == 0 && gp_setting(s, sl, small->hash) == 1); free(s);
    assert(games_drop_8gib(NULL, "test") == 0);                                     /* nothing left to turn off */

    assert(!game_set_patch("CUSA00900", big->hash, 1, 1, msg, sizeof msg));
    assert(games_drop_8gib("CUSA00900", "test") == 1);                              /* one game */
    s = settings_read(&sl);
    assert(gp_setting(s, sl, big->hash) == 0 && gp_setting(s, sl, small->hash) == 1); free(s);
    free(bb); free(kz);

    /* A collection: one resolution per AppElf; enabling one turns off only the other entry of the same executable. */
    const char *col = "<Patch>\n"
        "<Metadata Title=\"C\" Name=\"1440p (PS4 Pro mode)\" AppVer=\"01.00\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C\" Name=\"1800p (PS4 Pro mode)\" AppVer=\"01.00\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C2\" Name=\"Game 2 - 1440p (PS4 Pro mode)\" AppVer=\"01.00\" AppElf=\"game2.elf\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C2\" Name=\"Game 2 - 1800p (PS4 Pro mode)\" AppVer=\"01.00\" AppElf=\"GAME2.elf\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C\" Name=\"Offline mode\" AppVer=\"01.00\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n</Patch>\n";
    put(PS_DIR "/col.xml", col, strlen(col));
    install_game("CUSA99998", "C", "01.00", PS_DIR, "col.xml");
    int nc; gp_patch *c = game_patches("CUSA99998", "01.00", &nc);
    assert(nc == 5 && !strcmp(c[0].elf, "eboot.bin") && !strcmp(c[2].elf, "game2.elf") && !c[4].resolution);
    for (int i = 0; i < 5; i++) assert(!game_set_patch("CUSA99998", c[i].hash, 1, 1, msg, sizeof msg));
    s = settings_read(&sl);
    assert(!gp_setting(s, sl, c[0].hash) && gp_setting(s, sl, c[1].hash) == 1);       /* eboot.bin: the last one */
    assert(!gp_setting(s, sl, c[2].hash) && gp_setting(s, sl, c[3].hash) == 1);       /* AppElf compared ignoring case */
    assert(gp_setting(s, sl, c[4].hash) == 1); free(s);                               /* not a resolution entry */
    assert(!game_set_patch("CUSA99998", c[0].hash, 1, 1, msg, sizeof msg));
    s = settings_read(&sl);
    assert(gp_setting(s, sl, c[0].hash) == 1 && !gp_setting(s, sl, c[1].hash) && gp_setting(s, sl, c[3].hash) == 1); free(s);

    /* The collection starts in eboot.bin: turning off its last resolution turns off the other executables' ones, and
       a resolution of another executable turns on the eboot.bin one of the same name (else the first listed). */
    assert(gp_main_entry(c, nc, 3) == 0 && gp_main_entry(c, nc, 0) == -1 && gp_main_entry(c, nc, 4) == -1);
    assert(!game_set_patch("CUSA99998", c[0].hash, 0, 1, msg, sizeof msg) && strstr(msg, ". A PS4 Pro patch for C is required for C2") && !strstr(msg, "C2 and"));
    s = settings_read(&sl);
    assert(!gp_setting(s, sl, c[0].hash) && !gp_setting(s, sl, c[3].hash) && gp_setting(s, sl, c[4].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99998", c[2].hash, 1, 1, msg, sizeof msg) && strstr(msg, ". A PS4 Pro patch for C is required for C2, so 1440p (PS4 Pro mode) was enabled too"));
    s = settings_read(&sl);
    assert(gp_setting(s, sl, c[0].hash) == 1 && !gp_setting(s, sl, c[1].hash) && gp_setting(s, sl, c[2].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99998", c[3].hash, 1, 1, msg, sizeof msg) && !strstr(msg, "also"));   /* eboot.bin already on */
    s = settings_read(&sl);
    assert(gp_setting(s, sl, c[0].hash) == 1 && !gp_setting(s, sl, c[2].hash) && gp_setting(s, sl, c[3].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99998", c[1].hash, 1, 1, msg, sizeof msg) && !strstr(msg, "Also"));   /* eboot.bin switch */
    s = settings_read(&sl);
    assert(!gp_setting(s, sl, c[0].hash) && gp_setting(s, sl, c[1].hash) == 1 && gp_setting(s, sl, c[3].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99998", c[1].hash, 0, 1, msg, sizeof msg));
    assert(!game_set_patch("CUSA99998", c[3].hash, 1, 1, msg, sizeof msg));                            /* 1800p: same resolution */
    assert(gp_same_resolution("A - 1440p (PS4 Pro mode)", "1440p (PS4 Pro mode, x)") && !gp_same_resolution("A - 1440p (PS4 Pro mode)", "B - 1800p (PS4 Pro mode)"));
    s = settings_read(&sl); assert(gp_setting(s, sl, c[1].hash) == 1 && !gp_setting(s, sl, c[0].hash)); free(s);
    free(c);

    /* 8 GiB in a collection: the budget is set when eboot.bin starts, so an 8 GiB entry of another executable brings
       the 8 GiB eboot.bin entry, replacing one without 8 GiB; entries without 8 GiB keep picking one without it. */
    const char *col8 = "<Patch>\n"
        "<Metadata Title=\"C\" Name=\"4K (PS4 Pro mode)\" NeoRequires=\"pro\" AppVer=\"01.00\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C\" Name=\"4K (PS4 Pro mode, requires 8 GiB enabled)\" NeoRequires=\"pro,8gib\" AppVer=\"01.00\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C2\" Name=\"Game 2 - 4K (PS4 Pro mode)\" NeoRequires=\"pro\" AppVer=\"01.00\" AppElf=\"game2.elf\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C2\" Name=\"Game 2 - 4K (PS4 Pro mode, requires 8 GiB enabled)\" NeoRequires=\"pro,8gib\" AppVer=\"01.00\" AppElf=\"game2.elf\"><PatchList/></Metadata>\n"
        "<Metadata Title=\"C2\" Name=\"Game 2 - 1440p (PS4 Pro mode, requires 8 GiB enabled)\" NeoRequires=\"pro,8gib\" AppVer=\"01.00\" AppElf=\"game2.elf\"><PatchList/></Metadata>\n</Patch>\n";
    put(PS_DIR "/col8.xml", col8, strlen(col8));
    install_game("CUSA99997", "C", "01.00", PS_DIR, "col8.xml");
    gp_patch *e = game_patches("CUSA99997", "01.00", &nc);
    assert(nc == 5 && !e[0].needs_8gib && e[1].needs_8gib && e[3].needs_8gib);
    assert(!game_set_patch("CUSA99997", e[2].hash, 1, 1, msg, sizeof msg));            /* no 8 GiB: eboot.bin without */
    s = settings_read(&sl); assert(gp_setting(s, sl, e[0].hash) == 1 && !gp_setting(s, sl, e[1].hash)); free(s);
    assert(!game_set_patch("CUSA99997", e[3].hash, 1, 1, msg, sizeof msg) && strstr(msg, "C2 needs 8GB Mode from the start of the collection"));
    s = settings_read(&sl);                                                            /* upgraded, Game 2 switched */
    assert(!gp_setting(s, sl, e[0].hash) && gp_setting(s, sl, e[1].hash) == 1 && !gp_setting(s, sl, e[2].hash) && gp_setting(s, sl, e[3].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99997", e[4].hash, 1, 1, msg, sizeof msg) && !strstr(msg, "enabled instead"));   /* already 8 GiB */
    assert(!game_set_patch("CUSA99997", e[2].hash, 1, 1, msg, sizeof msg));            /* back without 8 GiB: eboot.bin kept */
    s = settings_read(&sl); assert(gp_setting(s, sl, e[1].hash) == 1 && gp_setting(s, sl, e[2].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99997", e[1].hash, 0, 1, msg, sizeof msg));            /* last eboot.bin off: all off */
    assert(!game_set_patch("CUSA99997", e[4].hash, 1, 1, msg, sizeof msg));            /* 8 GiB first, other resolution */
    s = settings_read(&sl); assert(!gp_setting(s, sl, e[0].hash) && gp_setting(s, sl, e[1].hash) == 1); free(s);
    assert(!game_set_patch("CUSA99997", e[1].hash, 0, 1, msg, sizeof msg));
    assert(game_set_patch("CUSA99997", e[3].hash, 1, 0, msg, sizeof msg) == -1);       /* refused without 8 GiB */
    free(e);
    /* Versions shown in the panel: without an update, the appmeta APP_VER; with one, the update's APP_VER from the
       appmeta param.sfo and the game's from the param.sfo inside app.pkg (entry 0x1000). */
    const char *vx = "<Patch>\n<Metadata Title=\"V\" Name=\"4K (PS4 Pro mode)\" AppVer=\"01.00, 01.05\" AppElf=\"eboot.bin\"><PatchList/></Metadata>\n</Patch>\n";
    put(PS_DIR "/vx.xml", vx, strlen(vx));
    install_game("CUSA99996", "V", "01.00", PS_DIR, "vx.xml");
    int ng; game_t *gs = games_scan(&ng), *v = NULL;
    for (int i = 0; i < ng; i++) if (!strcmp(gs[i].tid, "CUSA99996")) v = &gs[i];
    assert(v && !v->update && !strcmp(v->game_ver, "01.00") && !v->update_ver[0]);
    games_free(gs, ng);
    size_t sfo_len; char *sfo = ps_read(APPMETA_DIR "/CUSA99996/param.sfo", 4096, &sfo_len);
    unsigned char pkg[0x40 + 256] = {0x7f, 0x43, 0x4e, 0x54};
    assert(sfo && sfo_len <= 256);
    pkg[0x13] = 1; pkg[0x1b] = 0x20;                                       /* one entry, table at 0x20 */
    pkg[0x22] = 0x10; pkg[0x33] = 0x40; pkg[0x36] = (unsigned char)(sfo_len >> 8); pkg[0x37] = (unsigned char)sfo_len; /* id 0x1000, offset 0x40, size */
    memcpy(pkg + 0x40, sfo, sfo_len); free(sfo);
    assert(!system("mkdir -p " APP_DIR "/CUSA99996 " PATCH_DIR "/CUSA99996"));
    put(APP_DIR "/CUSA99996/app.pkg", pkg, 0x40 + sfo_len);
    install_game("CUSA99996", "V", "01.05", PS_DIR, "vx.xml");               /* the update's param.sfo */
    gs = games_scan(&ng); v = NULL;
    for (int i = 0; i < ng; i++) if (!strcmp(gs[i].tid, "CUSA99996")) v = &gs[i];
    assert(v && v->update && !strcmp(v->game_ver, "01.00") && !strcmp(v->update_ver, "01.05"));
    games_free(gs, ng);
    assert(!system("rm " APP_DIR "/CUSA99996/app.pkg"));                    /* no package: the master VERSION */
    gs = games_scan(&ng); v = NULL;
    for (int i = 0; i < ng; i++) if (!strcmp(gs[i].tid, "CUSA99996")) v = &gs[i];
    assert(v && !strcmp(v->game_ver, "01.00") && !strcmp(v->update_ver, "01.05"));
    games_free(gs, ng);
    /* A game on extended storage: its appmeta is in external/. Without its app folder on a mounted drive (the system
       keeps external/ after the game is moved or deleted) it is not found; with it, it is found and listed once. */
    install_game("CUSA00054", "Mad Max", "01.06", argv[1], "MadMax_CUSA00054.xml");
    assert(!system("mkdir -p " APPMETA_EXTERNAL " && mv " APPMETA_DIR "/CUSA00054 " APPMETA_EXTERNAL "/"));
    assert(game_info("CUSA00054", name, sizeof name, ver, sizeof ver));
    int ne, found = 0; game_t *ge = games_scan(&ne);
    for (int i = 0; i < ne; i++) found += !strcmp(ge[i].tid, "CUSA00054");
    assert(found == 0);
    games_free(ge, ne);
    assert(!system("mkdir -p " ROOT "/ext1/user/app/CUSA00054"));
    assert(!game_info("CUSA00054", name, sizeof name, ver, sizeof ver));
    assert(!strcmp(name, "Mad Max") && !strcmp(ver, "01.06"));
    ge = games_scan(&ne);
    for (int i = 0; i < ne; i++) found += !strcmp(ge[i].tid, "CUSA00054");
    assert(found == 1);
    games_free(ge, ne);

    puts("PASS 8 GiB patches turned off while 8 GiB is unavailable, other patches kept, one resolution per executable, "
         "games on extended storage");
    return 0;
}
