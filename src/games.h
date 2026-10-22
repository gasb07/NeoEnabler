/* GPL-3.0-only. Installed PS4 games with NeoEnabler patches, what a launch needs (Pro mode, 8 GiB) from the patches
   enabled in the ps-patch-system settings, and changes to those settings. Needs patch_sync.h (ps_read, ps_write,
   PS_DIR) and log_line(). */
#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "game_patches.h"

#ifndef APPMETA_DIR
#define APPMETA_DIR "/system_data/priv/appmeta"
#endif
#define APPMETA_EXTERNAL APPMETA_DIR "/external"   /* games installed on extended storage */
#ifndef APP_DIR
#define APP_DIR "/user/app"
#endif
#ifndef PATCH_DIR
#define PATCH_DIR "/user/patch"
#endif
#define SETTINGS_XML PS_DIR "/!_settings.xml"
#define GAME_MAX_PATCHES 128

typedef struct {
    char tid[10], name[128], version[16];
    char game_ver[16], update_ver[16];   /* APP_VER of the game package and of the installed update ("" when none) */
    int update;                 /* /user/patch/<title ID> exists */
    int count;
    gp_patch *patches;
} game_t;

/* Title IDs whose patches live in xml_orbis (ps-patch-system folder_for_title_id). */
static int game_tid(const char *s) {
    return strlen(s) == 9 && (!memcmp(s, "CUSA", 4) || !memcmp(s, "CUSB", 4)) &&
           strspn(s + 4, "0123456789") == 5;
}

/* Appmeta param.sfo of an installed game, from the internal folder or, for a game on extended storage, from
   external/; malloc'd, NULL when neither is readable. path (when not NULL) gets the file read or tried last. */
static char *game_sfo(const char *tid, size_t *n, char *path, size_t path_cap) {
    char p[96];
    snprintf(p, sizeof p, APPMETA_DIR "/%.9s/param.sfo", tid);
    char *sfo = ps_read(p, 64 << 10, n);
    if (!sfo) {
        snprintf(p, sizeof p, APPMETA_EXTERNAL "/%.9s/param.sfo", tid);
        sfo = ps_read(p, 64 << 10, n);
    }
    if (path) snprintf(path, path_cap, "%s", p);
    return sfo;
}

/* Name and version (the one ps-patch-system matches) of an installed game; 0 when its param.sfo is readable. */
static int game_info(const char *tid, char *name, size_t name_cap, char *ver, size_t ver_cap) {
    size_t n;
    char *sfo = game_sfo(tid, &n, NULL, 0);
    if (!sfo) return -1;
    char app[16], version[16];
    gp_sfo((unsigned char *)sfo, n, "TITLE", name, name_cap);
    gp_sfo((unsigned char *)sfo, n, "APP_VER", app, sizeof app);
    gp_sfo((unsigned char *)sfo, n, "VERSION", version, sizeof version);
    free(sfo);
    snprintf(ver, ver_cap, "%s", gp_game_version(app, version));
    return ver[0] ? 0 : -1;
}

static uint32_t game_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* APP_VER of the param.sfo inside a PS4 package (entry 0x1000); 0 when found. */
static int game_pkg_version(const char *path, char *ver, size_t cap) {
    int fd = open(path, O_RDONLY), rc = -1;
    if (fd < 0) return -1;
    unsigned char head[0x20], e[32];
    if (pread(fd, head, sizeof head, 0) == (ssize_t)sizeof head && game_be32(head) == 0x7f434e54) {
        uint32_t count = game_be32(head + 0x10), table = game_be32(head + 0x18);
        for (uint32_t i = 0; i < count && i < 4096 && rc; i++) {
            if (pread(fd, e, sizeof e, (off_t)table + (off_t)i * 32) != (ssize_t)sizeof e) break;
            if (game_be32(e) != 0x1000) continue;
            uint32_t size = game_be32(e + 20);
            unsigned char *sfo = size && size <= (64u << 10) ? malloc(size) : NULL;
            if (sfo && pread(fd, sfo, size, game_be32(e + 16)) == (ssize_t)size) {
                gp_sfo(sfo, size, "APP_VER", ver, cap);
                rc = ver[0] ? 0 : -1;
            }
            free(sfo);
            break;
        }
    }
    close(fd);
    return rc;
}

/* Game and update versions. With an update, the appmeta param.sfo is the update's, so the game version comes from
   the game package; when it cannot be read (a game mounted from another drive leaves /user/app empty), from the
   master VERSION of that param.sfo. Without an update it is the appmeta APP_VER. */
static void game_versions(game_t *g) {
    char path[96]; size_t n;
    g->game_ver[0] = g->update_ver[0] = 0;
    char app[16] = "", master[16] = "", *sfo = game_sfo(g->tid, &n, NULL, 0);
    if (sfo) {
        gp_sfo((unsigned char *)sfo, n, "APP_VER", app, sizeof app);
        gp_sfo((unsigned char *)sfo, n, "VERSION", master, sizeof master);
        free(sfo);
    }
    if (!g->update) { snprintf(g->game_ver, sizeof g->game_ver, "%s", app); return; }
    snprintf(g->update_ver, sizeof g->update_ver, "%s", app);
    snprintf(path, sizeof path, APP_DIR "/%.9s/app.pkg", g->tid);
    if (game_pkg_version(path, g->game_ver, sizeof g->game_ver)) snprintf(g->game_ver, sizeof g->game_ver, "%s", master);
}

/* Entries of the game's patch file that apply to its installed version; malloc'd, count in *n. */
static gp_patch *game_patches(const char *tid, const char *ver, int *n) {
    char path[96], file[16]; size_t len;
    *n = 0;
    snprintf(file, sizeof file, "%.9s.xml", tid);
    snprintf(path, sizeof path, PS_DIR "/%s", file);
    char *xml = ps_read(path, PS_MAX_FILE, &len);
    if (!xml) return NULL;
    gp_patch *p = malloc(sizeof(gp_patch) * GAME_MAX_PATCHES);
    if (p) *n = gp_list(xml, len, file, ver, p, GAME_MAX_PATCHES);
    free(xml);
    return p;
}

static void games_free(game_t *g, int n) {
    for (int i = 0; i < n; i++) free(g[i].patches);
    free(g);
}

static int game_cmp(const void *a, const void *b) {
    const game_t *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->tid, y->tid);
}

/* Installed games that have at least one patch needing Pro mode for their version, sorted by name. Games on
   extended storage have their appmeta in external/. */
static game_t *games_scan(int *count) {
    *count = 0;
    int cap = 32, n = 0; struct dirent *e;
    game_t *g = malloc(sizeof(game_t) * (size_t)cap);
    if (!g) return NULL;
    static const char *const dirs[] = {APPMETA_DIR, APPMETA_EXTERNAL};
    for (size_t k = 0; k < sizeof dirs / sizeof *dirs; k++) {
        DIR *d = opendir(dirs[k]);
        if (!d) continue;
        while ((e = readdir(d))) {
            if (!game_tid(e->d_name)) continue;
            int seen = 0;
            for (int i = 0; i < n && !seen; i++) seen = !strncmp(g[i].tid, e->d_name, 9);
            if (seen) continue;
            game_t x = {0};
            snprintf(x.tid, sizeof x.tid, "%.9s", e->d_name);
            char path[96]; struct stat st;
            snprintf(path, sizeof path, PS_DIR "/%s.xml", x.tid);
            if (stat(path, &st) || game_info(x.tid, x.name, sizeof x.name, x.version, sizeof x.version)) continue;
            x.patches = game_patches(x.tid, x.version, &x.count);
            int pro = 0;
            for (int i = 0; i < x.count; i++) pro |= x.patches[i].needs_pro;
            if (!pro) { free(x.patches); continue; }
            snprintf(path, sizeof path, PATCH_DIR "/%s", x.tid);
            x.update = !stat(path, &st) && S_ISDIR(st.st_mode);
            game_versions(&x);
            if (n == cap) {
                game_t *bigger = realloc(g, sizeof(game_t) * (size_t)(cap *= 2));
                if (!bigger) { free(x.patches); break; }
                g = bigger;
            }
            g[n++] = x;
        }
        closedir(d);
    }
    qsort(g, (size_t)n, sizeof(game_t), game_cmp);
    *count = n;
    return g;
}

static char *settings_read(size_t *len) {
    char *s = ps_read(SETTINGS_XML, PS_MAX_FILE, len);
    if (!s) { s = calloc(1, 1); *len = 0; }
    return s;
}

/* What a launch of this title needs, from its enabled patches: bit 0 Pro mode, bit 1 8 GiB. */
enum { NEEDS_PRO = 1, NEEDS_8GIB = 2 };
static int game_needs(const char *tid) {
    char name[128], ver[16];
    if (!game_tid(tid) || game_info(tid, name, sizeof name, ver, sizeof ver)) return 0;
    int n, needs = 0; size_t sl;
    gp_patch *p = game_patches(tid, ver, &n);
    char *s = settings_read(&sl);
    for (int i = 0; i < n; i++)
        if (gp_setting(s, sl, p[i].hash) == 1) needs |= (p[i].needs_pro ? NEEDS_PRO : 0) | (p[i].needs_8gib ? NEEDS_8GIB | NEEDS_PRO : 0);
    free(p); free(s);
    return needs;
}

/* Sets one entry in a settings text being edited; logs why. Returns gp_setting_set's result. */
static int game_flip(char **out, size_t *ol, const char *tid, const gp_patch *p, int on, const char *why) {
    char *next; size_t next_len;
    if (gp_setting_set(*out, *ol, p->hash, on, &next, &next_len)) return -1;
    free(*out); *out = next; *ol = next_len;
    log_line("patches: %s: %s %s (%s)", tid, p->name, on ? "enabled" : "disabled", why);
    return 0;
}

/* Enables or disables one patch of a game. Enabling a NeoEnabler resolution entry disables the other NeoEnabler
   resolution entries of that game for the same AppElf, so at most one is on per executable (a collection gets one per
   game it contains); other patches are not affected. In a collection the title ID starts in eboot.bin, which sets
   PS4 Pro mode: enabling a resolution of another executable also enables the eboot.bin resolution with the same 8 GiB
   need and resolution (gp_same_resolution; the first one listed when none matches) when no eboot.bin resolution is on,
   or when the entry needs 8 GiB and the eboot.bin one does not (it is replaced), and disabling the last eboot.bin
   resolution also disables the resolutions of the other executables. Refuses hashes that are not an entry of the
   game's installed version, and 8 GiB patches when 8 GiB is unavailable (unsupported firmware or no hypervisor
   exploit). Returns 0, or -1 with a reason in msg. */
static int game_set_patch(const char *tid, const char *hash, int on, int memory_ok, char *msg, size_t cap) {
    char name[128], ver[16];
    if (!game_tid(tid) || game_info(tid, name, sizeof name, ver, sizeof ver)) { snprintf(msg, cap, "Unknown game"); return -1; }
    int n; gp_patch *p = game_patches(tid, ver, &n), *hit = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(p[i].hash, hash)) hit = &p[i];
    if (!hit) { free(p); snprintf(msg, cap, "This patch is not available for %s %s", tid, ver); return -1; }
    if (on && hit->needs_8gib && !memory_ok) { free(p); snprintf(msg, cap, "This patch needs 8GB Mode: firmware 13.20 or 13.60 with the validated no-HV launch hook installed"); return -1; }
    size_t sl, ol; char *s = settings_read(&sl), *out = NULL;
    int rc = gp_setting_set(s, sl, hash, on, &out, &ol), others = 0, k = (int)(hit - p), main_i = gp_main_entry(p, n, k);
    char extra[640] = "";
    for (int i = 0; !rc && on && hit->resolution && i < n; i++) {
        if (&p[i] == hit || !p[i].resolution || strcasecmp(p[i].elf, hit->elf) || gp_setting(out, ol, p[i].hash) != 1) continue;
        rc = game_flip(&out, &ol, tid, &p[i], 0, "one resolution per executable");
        others++;
    }
    int main_on = 0, main_8gib = 0;
    for (int i = 0; i < n; i++)
        if (p[i].resolution && gp_main_elf(&p[i]) && gp_setting(out, ol, p[i].hash) == 1) { main_on = 1; main_8gib |= p[i].needs_8gib; }
    /* the other executables start through LoadExec with the budget of the eboot.bin launch: an 8 GiB entry of one of
       them replaces an eboot.bin resolution that does not need 8 GiB by one that does */
    int upgrade = on && main_i >= 0 && main_on && hit->needs_8gib && !main_8gib;
    if (!rc && on && main_i >= 0 && (!main_on || upgrade)) {
        /* the eboot.bin resolution with the same 8 GiB need, then the same name, else the first one listed; never an
           8 GiB one without 8 GiB, and only an 8 GiB one for an upgrade */
        int pick = -1, best = -1;
        for (int i = 0; i < n; i++) {
            if (!p[i].resolution || !gp_main_elf(&p[i]) || (p[i].needs_8gib && !memory_ok) || (upgrade && !p[i].needs_8gib)) continue;
            int score = 2 * (!p[i].needs_8gib == !hit->needs_8gib) + gp_same_resolution(p[i].name, hit->name);
            if (score > best) { best = score; pick = i; }
        }
        if (pick < 0) rc = upgrade ? 0 : -2;
        else {
            for (int i = 0; !rc && upgrade && i < n; i++)
                if (i != pick && p[i].resolution && gp_main_elf(&p[i]) && gp_setting(out, ol, p[i].hash) == 1)
                    rc = game_flip(&out, &ol, tid, &p[i], 0, "replaced by an 8GB Mode eboot.bin resolution");
            if (!rc && !(rc = game_flip(&out, &ol, tid, &p[pick], 1, upgrade ? "8GB Mode is set when eboot.bin starts" : "the collection starts in eboot.bin"))) {
                if (upgrade) snprintf(extra, sizeof extra, ". %s needs 8GB Mode from the start of the collection, so %s was enabled instead",
                                      hit->title, p[pick].name);
                else snprintf(extra, sizeof extra, ". A PS4 Pro patch for %s is required for %s, so %s was enabled too",
                              p[pick].title, hit->title, p[pick].name);
            }
        }
    }
    if (!rc && !on && hit->resolution && gp_main_elf(hit) && !main_on) {
        int dropped = 0;
        for (int i = 0; !rc && i < n; i++) {
            if (gp_main_entry(p, n, i) < 0 || gp_setting(out, ol, p[i].hash) != 1) continue;
            if ((rc = game_flip(&out, &ol, tid, &p[i], 0, "no eboot.bin resolution is on"))) break;
            int seen = 0;               /* each game once: "... is required for <game> and <game>" */
            for (int j = 0; j < i && !seen; j++)
                seen = gp_main_entry(p, n, j) >= 0 && !strcmp(p[j].title, p[i].title) && gp_setting(s, sl, p[j].hash) == 1;
            if (seen) continue;
            size_t at = strlen(extra);
            if (!dropped++) snprintf(extra + at, sizeof extra - at, ". A PS4 Pro patch for %s is required for %s",
                                     hit->title, p[i].title);
            else snprintf(extra + at, sizeof extra - at, " and %s", p[i].title);
        }
    }
    if (rc == -2) snprintf(msg, cap, "%s: %s not enabled. A PS4 Pro patch for %s is required for %s, and none can be enabled",
                           tid, hit->name, p[main_i].title, hit->title);
    else if (rc) snprintf(msg, cap, "The patch system settings file is malformed; nothing changed");
    else if (ps_write(PS_DIR, "!_settings.xml", out, ol)) { snprintf(msg, cap, "Writing the settings failed (errno %d)", errno); rc = -1; }
    else snprintf(msg, cap, "%s: %s %s%s%s", tid, hit->name, on ? "enabled" : "disabled",
                  others ? "; the other resolution was disabled" : "", extra);
    log_line("patches: %s", msg);
    free(out); free(s); free(p);
    return rc ? -1 : 0;
}

/* Turns off the enabled patches that need 8 GiB, of one game or, with only_tid NULL, of every game. For use while 8 GiB
   is not available: such a patch would run the game in Pro mode with the original memory. `why` goes to the log.
   Returns how many were turned off. */
static int games_drop_8gib(const char *only_tid, const char *why) {
    int n, dropped = 0; game_t *g = games_scan(&n);
    size_t sl; char *settings = settings_read(&sl);
    for (int i = 0; i < n; i++) {
        if (only_tid && strcmp(g[i].tid, only_tid)) continue;
        for (int k = 0; k < g[i].count; k++) {
            const gp_patch *p = &g[i].patches[k];
            char msg[2048];
            if (!p->needs_8gib || !settings || gp_setting(settings, sl, p->hash) != 1) continue;
            if (!game_set_patch(g[i].tid, p->hash, 0, 0, msg, sizeof msg)) {
                dropped++;
                log_line("patches: %s: \"%s\" turned off, it needs 8GB Mode (%s)", g[i].tid, p->name, why);
            }
        }
    }
    free(settings); games_free(g, n);
    return dropped;
}
