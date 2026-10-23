/* GPL-3.0-only. NEO Enabler manager: installs the NEO launch hook in SceShellCore, shows a PS5 notification and
   stays resident with a web page on port 8765 that lists the installed games with NeoEnabler patches and switches
   them in the ps-patch-system settings. At every PS4 launch the hook asks the manager what the game needs: Pro mode
   and the guarded 8 GiB NEO budget follow the enabled patches, and the budget returns to the original after the
   game closes. Also installs the Media tab shortcut (NEOE00001) when missing. Hook logic in shellcore_hook.h. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <netinet/in.h>
#include <pthread.h>
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include "shellcore_hook.h"
#include "attr_detect.h"
#include "legacy_v4.h"
#include "icon_data.h"
#include "panel_data.h"
static void log_line(const char *fmt, ...);
#include "memory_control.h"
#include "gpu_control.h"
#include "settings.h"
#include "http_request.h"
#include "log_mirror.h"

#define PORT 8765
#define VERSION "NeoEnabler 0.5.0"
typedef struct {
    uint32_t app_id; uint64_t unknown1; char title_id[10]; uint32_t apptype; char unknown2[8];
    uint32_t attribute; /* param.sfo ATTRIBUTE as launched, including the hook change */
    char unknown3[0x2a];
} app_info_t;
#define ATTR_PS4_PRO 0x00800000
typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *user);
int sceSystemServiceLaunchApp(const char *title_id, const char **argv, void *param);

/* NeoEnabler's own log. The kernel log keeps only the last 128 KiB, which the system overwrites within seconds, so
   every line also goes here, with the console time to the millisecond and the seconds since start. At 256 KiB the file becomes neo.log.old and a new one starts. */
#define NEO_LOG_DIR "/data/neo-enabler"
#define NEO_LOG_PATH NEO_LOG_DIR "/neo.log"
#define NEO_LOG_OLD NEO_LOG_PATH ".old"
#define NEO_LOG_MAX (256 * 1024)
/* The kernel log without the noise and without personal ids, kept as it goes by (the ring holds 128 KiB). Three
   files of at most 30 minutes or 4 MiB each: kernel.log.2 (oldest), kernel.log.1 and kernel.log, so at least an
   hour is kept unless a file hits its size cap first. */
#define KLOG_PATH NEO_LOG_DIR "/kernel.log"
#define KLOG_OLD1 KLOG_PATH ".1"
#define KLOG_OLD2 KLOG_PATH ".2"
#define KLOG_MAX (4 * 1024 * 1024)
#define KLOG_CHUNK_MS (30LL * 60 * 1000)

static struct timespec manager_start; /* monotonic clock at the first log line */

/* "YYYY-MM-DD HH:MM:SS.mmm +S.SSSs ". The console clock is often wrong (2012-01-01 after a boot without network
   time), so every line also has the seconds since the manager started, from the monotonic clock. */
static size_t log_stamp(char *out, size_t cap) {
    struct timespec wall, mono; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &wall); clock_gettime(CLOCK_MONOTONIC, &mono);
    if (!manager_start.tv_sec && !manager_start.tv_nsec) manager_start = mono;
    double up = (double)(mono.tv_sec - manager_start.tv_sec) + (double)(mono.tv_nsec - manager_start.tv_nsec) / 1e9;
    time_t t = wall.tv_sec; gmtime_r(&t, &tm);
    size_t k = strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
    k += (size_t)snprintf(out + k, cap - k, ".%03d +%.3fs ", (int)(wall.tv_nsec / 1000000), up);
    return k;
}

/* Appends to a log file and fsyncs. Past `max` bytes the file becomes `old` and a new one starts. */
static void append_log(const char *path, const char *old, size_t max, const char *data, size_t n) {
    static int dir_ready;
    if (!dir_ready) { mkdir(NEO_LOG_DIR, 0777); dir_ready = 1; }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    if (write(fd, data, n) < 0) { close(fd); return; }
    fsync(fd);
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    if (old && (size_t)size > max) { unlink(old); rename(path, old); }
}

static void file_log(const char *kind, const char *line, size_t n) {
    char out[1100];
    size_t k = log_stamp(out, 48);
    k += (size_t)snprintf(out + k, sizeof(out) - k - 1, "%s%.*s", kind, (int)(n < 900 ? n : 900), line);
    out[k++] = '\n';
    append_log(NEO_LOG_PATH, NEO_LOG_OLD, NEO_LOG_MAX, out, k);
}

/* Every action and its result go to the kernel log (kern.msgbuf) and to NeoEnabler's own log file. */
static void log_line(const char *fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    klog_printf("[NeoEnabler] %s\n", line);
    file_log("[NeoEnabler] ", line, strlen(line));
}

static void notify(const char *msg) {
    log_line("%s", msg);
    notify_request_t req; memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#define TITLE_ID "NEOE00001"

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);


/* libSceAppInstUtil entry points. Loaded with the dependency chain used by ps5-app-dumper (each module
   loaded before the next one, then UserService/NetCtl/AppInstUtil initialized): loading AppInstUtil
   alone stops the payload on this loader. */
static struct {
    int (*exists)(const char *, uint8_t *);                  /* sceAppInstUtilAppExists */
    int (*install_dir)(const char *, const char *, void *); /* sceAppInstUtilAppInstallTitleDir (absent on fw 13.x) */
    int (*install_all)(void *);                              /* sceAppInstUtilAppInstallAll */
} appinst;

/* Returns 0 when ready, -1 missing library, -2 missing symbol, or the sceAppInstUtilInitialize error. */
static int appinst_open(void) {
    static int state = 1;
    if (state != 1) return state;
    static const char *libs[] = {"libSceIpmi.sprx", "libSceSsl.sprx", "libSceHttp2.sprx", "libSceSystemService.sprx",
                                 "libSceNetCtl.sprx", "libSceUserService.sprx", "libSceAppInstUtil.sprx"};
    int h[7];
    for (int i = 0; i < 7; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/system/common/lib/%s", libs[i]);
        if ((h[i] = sceKernelLoadStartModule(path, 0, 0, 0, 0, 0)) < 0) return state = -1;
    }
    int (*user_init)(void *) = (void *)kernel_dynlib_resolve(-1, h[5], "j3YMu1MVNNo");  /* sceUserServiceInitialize */
    int (*netctl_init)(void) = (void *)kernel_dynlib_resolve(-1, h[4], "gky0+oaNM4k"); /* sceNetCtlInit */
    int (*init)(void) = (void *)kernel_dynlib_resolve(-1, h[6], "540lotO7oHE");        /* sceAppInstUtilInitialize */
    appinst.exists = (void *)kernel_dynlib_resolve(-1, h[6], "kUT4RpxclMQ");
    appinst.install_dir = (void *)kernel_dynlib_resolve(-1, h[6], "Wudg3Xe3heE");
    appinst.install_all = (void *)kernel_dynlib_resolve(-1, h[6], "+scQA5stvjs");
    if (!user_init || !netctl_init || !init || !appinst.exists || (!appinst.install_dir && !appinst.install_all)) return state = -2;
    uint32_t prio = 0x100;
    user_init(&prio);
    netctl_init();
    return state = init();
}

/* Media tab shortcut, same scheme as the other web panels on the console (category 65536 + deeplinkUri). */
static const char launcher_param[] =
    "{\n"
    "    \"titleId\": \"" TITLE_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:8765/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"NeoEnabler\"\n"
    "        }\n"
    "    }\n"
    "}\n";


static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *buf = malloc(size);
    int same = buf && fread(buf, 1, size, f) == size && !memcmp(buf, data, size);
    free(buf); fclose(f);
    return same;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

#include "patch_sync.h"
#include "games.h"
#include "module_patches.h"
static char patches_message[256] = "Not checked yet";

#define SETTINGS_PATH "/data/neo-settings"

/* Firmware support is independent of a running game or an active budget. */
/* Preserve the original firmware query during the hook diagnosis. */
static uint32_t fw_raw_version(void) {
    uint32_t v = 0; size_t n = sizeof(v);
    sysctlbyname("machdep.upd_version", &v, &n, NULL, 0);
    return v;
}
static int memory_supported(void) { return mb_find_layout(fw_raw_version()) != NULL; }
static int memory_community_report(void) { return fw_raw_version() == 0x13600000; }
/* Availability also requires a ready memory backend; an idle game is not required. */
static int memory_available(void) { mb_snapshot m; return memory_supported() && !memory_check(&m); }
/* GPU clock page and boost: the MP1 driver answering a clock read. */
static int gpu_available(void) { unsigned mhz; return gpu_backend_supported() && !gpu_backend_read(&mhz); }

/* Advanced overrides: Pro mode for every PS4 game, and 8 GiB kept on. */
static int force_neo, force_8gib;

static void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    char buf[1024] = {0};
    size_t used = fread(buf, 1, sizeof(buf) - 1, f);
    if (ferror(f) || !feof(f)) { fclose(f); log_line("settings: unreadable or oversized file ignored"); return; }
    if (used) {
        neo_settings cfg;
        if (neo_settings_parse(buf, &cfg)) log_line("settings: invalid GPU values; GPU boost disabled");
        force_neo = cfg.force_neo;
        force_8gib = cfg.force_8gib && memory_supported();
        gpu_configure(cfg.gpu_enabled, cfg.gpu_mhz);
        if (cfg.gpu_enabled && !gpu_backend_supported()) log_line("settings: saved GPU boost ignored on unsupported firmware");
        if (cfg.force_8gib && !memory_supported())
            log_line("settings: force_8gib=1 ignored, 8GB Mode is not supported on firmware %x.%02x",
                     (fw_raw_version() >> 24) & 0xff, (fw_raw_version() >> 16) & 0xff);
        if (cfg.memory && !cfg.force_8gib) log_line("settings: memory_8gib=1 of an older version ignored; 8GB Mode now follows the game patches");
    }
    fclose(f);
}

static int settings_save(void) {
    neo_settings cfg = {.neo = 1, .memory = force_8gib, .gpu_enabled = gpu.enabled, .gpu_mhz = gpu.target,
                        .force_neo = force_neo, .force_8gib = force_8gib};
    char buf[128]; int n = neo_settings_format(&cfg, buf, sizeof(buf));
    if (n < 0 || write_file(SETTINGS_PATH ".tmp", buf, (size_t)n)) return -1;
    return rename(SETTINGS_PATH ".tmp", SETTINGS_PATH);
}

/* Returns 0 already installed, 1 installed now, <0 error. "Installed" means registered in the system
   (sceAppInstUtilAppExists) with the current param.json and icon: deleting the shortcut from the home
   screen makes the next start install it again. */
static int install_launcher(void) {
    const char *param_path = "/user/app/" TITLE_ID "/sce_sys/param.json", *icon_path = "/user/app/" TITLE_ID "/sce_sys/icon0.png";
    int err = appinst_open();
    if (err) { log_line("Media tab shortcut: appinst open rc=%x", err); return -3; }
    uint8_t exists = 0;
    if (!appinst.exists(TITLE_ID, &exists) && exists &&
        same_file(param_path, launcher_param, sizeof(launcher_param) - 1) && same_file(icon_path, icon0_png, sizeof(icon0_png))) return 0;
    if ((mkdir("/user/app/" TITLE_ID, 0755) && errno != EEXIST) || (mkdir("/user/app/" TITLE_ID "/sce_sys", 0755) && errno != EEXIST)) return -1;
    if (write_file(param_path, launcher_param, sizeof(launcher_param) - 1) || write_file(icon_path, icon0_png, sizeof(icon0_png))) return -2;
    err = appinst.install_dir ? appinst.install_dir(TITLE_ID, "/user/app/", 0) : appinst.install_all(0);
    log_line("Media tab shortcut: install %s rc=%x", TITLE_ID, err);
    return err ? -3 : 1;
}

/* Game name from the TITLE key of /system_data/priv/appmeta/<title id>/param.sfo; empty when not found. */
static void game_name(const char *title_id, char *out, size_t cap) {
    char path[64]; unsigned char sfo[8192]; size_t n;
    out[0] = 0;
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%.9s/param.sfo", title_id);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    n = fread(sfo, 1, sizeof(sfo), f); fclose(f);
    if (n < 20 || memcmp(sfo, "\0PSF", 4)) return;
    uint32_t keys, data, count;
    memcpy(&keys, sfo + 8, 4); memcpy(&data, sfo + 12, 4); memcpy(&count, sfo + 16, 4);
    for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= n; i++) {
        const unsigned char *e = sfo + 20 + i * 16;
        uint16_t key_off; uint32_t len, data_off;
        memcpy(&key_off, e, 2); memcpy(&len, e + 4, 4); memcpy(&data_off, e + 12, 4);
        if (keys + key_off + 6 > n || memcmp(sfo + keys + key_off, "TITLE", 6)) continue;
        if (data + data_off + len > n) return;
        size_t m = len < cap ? len : cap - 1;
        memcpy(out, sfo + data + data_off, m); out[m] = 0;
        return;
    }
}

/* Copies s into out with the HTML special characters escaped. */
static void html_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
        if (r) { size_t l = strlen(r); memcpy(out + o, r, l); o += l; } else out[o++] = *s;
    }
    out[o] = 0;
}

/* Mode of the last open PS4 game listed by games_text: -1 none, 0 base, 1 PS4 Pro. */
static int running_pro = -1;

/* Open PS4 games and whether they run in PS4 Pro mode. */
static int games_text(char *out, size_t cap, int html) {
    running_pro = -1;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; size_t n = 0; int len = 0, games = 0;
    out[0] = 0;
    if (sysctl(mib, 4, NULL, &n, NULL, 0) || !n) return 0;
    unsigned char *buf = malloc(n);
    if (!buf || sysctl(mib, 4, buf, &n, NULL, 0)) { free(buf); return 0; }
    for (size_t off = 0; off + sizeof(struct kinfo_proc) <= n;) {
        struct kinfo_proc *ki = (void *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > n - off) break;
        if (!strcmp(ki->ki_comm, "eboot.bin")) {
            app_info_t info = {0}; uint32_t h = 0;
            if (!sceKernelGetAppInfo(ki->ki_pid, &info) && info.title_id[0]) {
                /* The driver name gives the mode when the game loads a mode-specific driver. Some games (The Order: 1886 on 13.20) load
                   libSceGnmDriverCompat1 in both modes, so then the mode comes from the attribute. */
                int neo = !kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriverForNeoMode.sprx", &h);
                int ps4 = neo || !kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriver.sprx", &h);
                if (!ps4 && !kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriverCompat1.sprx", &h)) {
                    ps4 = 1;
                    neo = (info.attribute & ATTR_PS4_PRO) != 0;
                }
                if (ps4) running_pro = neo;
                if (ps4 && len < (int)cap - 400) {
                    char name[128], esc[384];
                    game_name(info.title_id, name, sizeof(name));
                    if (html) html_escape(name, esc, sizeof(esc)); else snprintf(esc, sizeof(esc), "%s", name);
                    games++;
                    char memory[80]="DMEM query unavailable";int dmib[24];size_t dc=23,bytes=8;uint64_t dmem=0;
                    if(!sysctlnametomib("kern.app.dmem_size_pid",dmib,&dc)&&dc<=23){
                        dmib[dc++]=ki->ki_pid;
                        if(!sysctl(dmib,(unsigned)dc,&dmem,&bytes,NULL,0)&&bytes==8)
                            snprintf(memory,sizeof memory,"DMEM: %llu MiB",(unsigned long long)(dmem>>20));
                    }
                    len += snprintf(out + len, cap - len, html ? "<li><b>%s</b> (%.9s): %s; %s</li>" : "%s (%.9s): %s; %s\n",
                                    esc[0] ? esc : "Unknown game", info.title_id, neo ? "PS4 Pro (NEO)" : "PS4 base",memory);
                }
            }
        }
        off += (size_t)ki->ki_structsize;
    }
    free(buf);
    if (!games) snprintf(out, cap, html ? "<li>No PS4 game running</li>" : "No PS4 game running\n");
    return games;
}

/* Title ID of the open PS4 game (an eboot.bin process with one); 1 when there is one. */
static int open_game(char tid[10]) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; size_t n = 0; int found = 0;
    tid[0] = 0;
    if (sysctl(mib, 4, NULL, &n, NULL, 0) || !n || n > 32 * 1024 * 1024) return 0;
    unsigned char *buf = malloc(n);
    if (!buf || sysctl(mib, 4, buf, &n, NULL, 0)) { free(buf); return 0; }
    for (size_t off = 0; !found && off + sizeof(struct kinfo_proc) <= n;) {
        struct kinfo_proc *ki = (void *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > n - off) break;
        app_info_t info = {0};
        if (!strcmp(ki->ki_comm, "eboot.bin") && !sceKernelGetAppInfo(ki->ki_pid, &info) && info.title_id[0]) {
            snprintf(tid, 10, "%.9s", info.title_id);
            found = 1;
        }
        off += (size_t)ki->ki_structsize;
    }
    free(buf);
    return found;
}

/* Play: the launch runs in its own thread, because ShellCore asks this manager about the launch (Pro mode, 8GB
   Mode) while it is starting the game, and the questions are answered by the main loop. The thread only calls the
   system service; the main loop logs and notifies the result (play_tick). */
static struct { volatile int busy, done; int rc; char tid[10]; } play;

static void *play_thread(void *arg) {
    (void)arg;
    struct { uint32_t size, user, opt; uint64_t report; uint32_t flag; } param = {0};
    int user = -1, rc = sceUserServiceGetForegroundUser(&user);
    if (!rc && user > 0) {
        param.size = sizeof param; param.user = (uint32_t)user;
        const char *argv[] = {NULL};
        rc = sceSystemServiceLaunchApp(play.tid, argv, &param);
    } else if (!rc) rc = -1;
    play.rc = rc;
    play.done = 1;
    return NULL;
}

/* Starts an installed game for the signed-in user. Refused while a game is open or another start is running. */
static int play_start(const char *tid, char *msg, size_t cap) {
    char open[10], name[128];
    if (!game_tid(tid)) { snprintf(msg, cap, "Unknown game"); return -1; }
    game_name(tid, name, sizeof name);
    if (!name[0]) snprintf(name, sizeof name, "%s", tid);
    if (play.busy) { snprintf(msg, cap, "%s is already starting", play.tid); return -1; }
    if (open_game(open)) {
        char other[128];
        game_name(open, other, sizeof other);
        if (!strcmp(open, tid)) snprintf(msg, cap, "%s is already open", name);
        else snprintf(msg, cap, "Close %s before starting %s", other[0] ? other : open, name);
        return -1;
    }
    snprintf(play.tid, sizeof play.tid, "%s", tid);
    play.busy = 1; play.done = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, play_thread, NULL)) { play.busy = 0; snprintf(msg, cap, "%s could not be started", name); return -1; }
    pthread_detach(th);
    log_line("play: starting %s (%s)", tid, name);
    snprintf(msg, cap, "Starting %s", name);
    return 0;
}

static void play_tick(void) {
    if (!play.busy || !play.done) return;
    play.busy = 0;
    log_line("play: %s: launch 0x%08x", play.tid, (unsigned)play.rc);
    if (play.rc < 0) {
        char note[200];
        snprintf(note, sizeof note, play.rc == -1 ? "%s could not be started: no user is signed in"
                                                  : "%s could not be started (0x%08x)", play.tid, (unsigned)play.rc);
        notify(note);
    }
}

/* A unique PS4 eboot is the only automatic clock target. Native games, mixed
 * game processes, failed scans and launcher processes never trigger boost. */
static int gpu_game_pid(void) {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; size_t n = 0;
    if (sysctl(mib, 4, NULL, &n, NULL, 0) || !n || n > 32 * 1024 * 1024) return -1;
    unsigned char *buf = malloc(n);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &n, NULL, 0)) { free(buf); return -1; }
    int candidate = 0, count = 0, valid = 1; size_t off = 0;
    while (off + sizeof(struct kinfo_proc) <= n) {
        struct kinfo_proc *ki = (void *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > n - off) { valid = 0; break; }
        if (!strcmp(ki->ki_comm, "eboot.bin")) {
            app_info_t info = {0}; uint32_t h = 0; count++;
            if (sceKernelGetAppInfo(ki->ki_pid, &info) || !info.title_id[0]) valid = 0;
            else if (!kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriverForNeoMode.sprx", &h) ||
                     !kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriver.sprx", &h) ||
                     !kernel_dynlib_handle(ki->ki_pid, "libSceGnmDriverCompat1.sprx", &h)) candidate = ki->ki_pid;
        }
        off += (size_t)ki->ki_structsize;
    }
    if (off != n || count > 1) valid = 0;
    free(buf); return valid ? candidate : -1;
}

static void send_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return; p += w; n -= (size_t)w; }
}

static void reply(int fd, const char *status, const char *type, const char *body) {
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, strlen(body));
    send_all(fd, head, (size_t)n); send_all(fd, body, strlen(body));
}

static void reply_bytes(int fd, const char *status, const char *type, const char *cache, const void *body, size_t n) {
    char head[320];
    int h = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\nConnection: close\r\n\r\n",
                     status, type, n, cache);
    send_all(fd, head, (size_t)h); send_all(fd, body, n);
}

/* Growing text buffer for pages whose size depends on the games installed. */
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_printf(sbuf *b, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int w = b->p ? vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap) : -1;
        va_end(ap);
        if (w >= 0 && (size_t)w < b->cap - b->n) { b->n += (size_t)w; return; }
        size_t cap = b->cap ? b->cap * 2 : 16384;
        if (w >= 0 && cap < b->n + (size_t)w + 1) cap = b->n + (size_t)w + 1024;
        char *p = realloc(b->p, cap);
        if (!p) return;
        b->p = p; b->cap = cap;
    }
}
static void sb_esc(sbuf *b, const char *s) {
    char tmp[1536];
    html_escape(s, tmp, sizeof tmp);
    sb_printf(b, "%s", tmp);
}

static const char page_head[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NeoEnabler</title><style>"
    ":root{color-scheme:dark;--bg:#080b12;--surface:#111722;--surface-2:#171f2d;--line:#263145;--text:#f4f7fb;"
    "--muted:#95a2b8;--blue:#55a7ff;--blue-strong:#147ee8;--green:#52d39a;--amber:#f2bd5d;--red:#ff6c79;--radius:14px}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;background:radial-gradient(circle at 15% -10%,rgba(20,126,232,.17),transparent 34rem),var(--bg);"
    "color:var(--text);font:15px/1.45 system-ui,-apple-system,'Segoe UI',sans-serif}"
    "header{position:sticky;top:0;z-index:2;display:flex;flex-wrap:wrap;gap:12px;align-items:center;justify-content:space-between;"
    "padding:14px 22px;background:rgba(8,11,18,.94);border-bottom:1px solid var(--line)}"
    ".brand{display:flex;align-items:center;gap:12px}.mark{display:grid;place-items:center;width:38px;height:38px;border-radius:11px;"
    "background:linear-gradient(145deg,#147ee8,#74baff);font-weight:800}"
    "h1{font-size:19px;margin:0}h1 small{color:var(--muted);font-weight:400;font-size:13px;margin-left:8px}"
    "nav{display:flex;gap:8px}nav a{color:var(--muted);text-decoration:none;padding:8px 14px;border-radius:10px;border:1px solid var(--line)}"
    "nav a[aria-current=page]{color:var(--text);background:var(--surface-2);border-color:var(--blue)}"
    "main{width:min(1400px,100%);margin:0 auto;padding:20px 16px}"
    ".layout{display:grid;grid-template-columns:minmax(0,1fr) 320px;gap:18px;align-items:start}"
    "@media(max-width:900px){.layout{grid-template-columns:1fr}}"
    ".card,.game{background:rgba(17,23,34,.96);border:1px solid var(--line);border-radius:var(--radius);padding:16px}"
    "aside{display:grid;gap:14px}aside h2{font-size:15px;margin:0 0 8px}"
    ".games{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:14px}"
    ".game{display:flex;flex-direction:column;padding:0;overflow:hidden}"
    ".game .art{width:100%;aspect-ratio:1/1;object-fit:cover;background:var(--surface-2);display:block}"
    ".info{display:flex;flex-direction:column;gap:2px;padding:10px 12px 0;flex:1}.name{font-size:15px}"
    ".meta{color:var(--muted);font-size:13px}.actions{display:flex;gap:8px;padding:0 12px 12px}.actions button{flex:1;margin:10px 0 0}"
    "button.primary{background:var(--blue-strong);border-color:var(--blue-strong)}"
    ".window{position:fixed;inset:0;z-index:5;display:grid;place-items:center;padding:16px;background:rgba(0,0,0,.66)}"
    ".window[hidden]{display:none}"
    ".sheet{width:min(860px,100%);max-height:calc(100vh - 32px);overflow:auto;background:var(--surface);border:1px solid var(--line);"
    "border-radius:var(--radius);padding:16px}"
    ".whead{display:flex;align-items:center;gap:12px}.whead img{width:56px;height:56px;border-radius:10px;object-fit:cover;"
    "background:var(--surface-2)}.whead h2{flex:1;font-size:18px;margin:0}.whead h2 span{color:var(--muted);font-weight:400;font-size:14px}"
    ".whead .close{margin:0}"
    ".facts{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:14px 0;padding:10px 12px;border:1px solid var(--line);"
    "border-radius:10px;background:var(--surface-2)}.facts dt{color:var(--muted);font-size:12px}.facts dd{margin:0;font-weight:600}"
    ".patches li[hidden]{display:none}"
    ".patches{list-style:none;margin:0;padding:0;display:grid;gap:8px}"
    ".patch{display:grid;grid-template-columns:22px minmax(0,1fr);gap:10px;padding:10px 12px;border:1px solid var(--line);"
    "border-radius:10px;background:var(--surface-2);cursor:pointer}"
    ".patch input{width:18px;height:18px;margin:2px 0 0;accent-color:var(--blue)}"
    ".patch:has(input:checked){border-color:var(--blue)}.patch:has(input:disabled){opacity:.5;cursor:default}"
    ".patch i{display:block;color:var(--muted);font-size:13px;margin-top:2px}"
    ".tag{display:inline-block;margin-left:8px;padding:1px 8px;border-radius:999px;font-size:11px;font-weight:600;"
    "color:var(--amber);border:1px solid rgba(242,189,93,.45);vertical-align:1px}"
    ".state{display:inline-block;padding:2px 10px;border-radius:999px;font-size:12px;font-weight:600;background:var(--surface-2)}"
    ".on{background:rgba(82,211,154,.18);color:var(--green)}.off{color:var(--muted)}"
    "button{font:inherit;color:var(--text);background:var(--surface-2);border:1px solid var(--line);border-radius:10px;"
    "padding:7px 13px;margin:8px 8px 0 0;cursor:pointer}button:hover{border-color:var(--blue)}button:disabled{opacity:.45;cursor:default}"
    "button.danger{background:#3a1f1d;border-color:#5c2c28}"
    "details summary{cursor:pointer;color:var(--muted);font-size:13px}details[open] summary{margin-bottom:8px}"
    "input[type=range]{width:100%;margin:20px 0}output{font-size:24px;font-weight:600}"
    ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}@media(max-width:600px){.grid{grid-template-columns:1fr}}"
    ".wide{grid-column:1/-1}.metric{font-size:32px;font-weight:600;margin:10px 0}.notice{margin:12px 0;color:var(--muted)}"
    ".msg{margin:0 0 14px;padding:10px 14px;border:1px solid var(--line);border-radius:10px;background:var(--surface)}"
    ".search{width:100%;min-height:42px;margin:0 0 14px;padding:0 14px;border:1px solid var(--line);border-radius:10px;"
    "background:#0c111a;color:var(--text);font:inherit}.search:focus{outline:2px solid var(--blue);outline-offset:1px}"
    ".game[hidden]{display:none}"
    "ul{margin:6px 0 0;padding-left:18px}small{color:var(--muted)}"
    "</style><script>/* the browser of older firmwares has no <dialog>: it gets the legacy panel */"
    "if(typeof document.createElement('dialog').showModal!=='function')location.replace('/legacy');</script>"
    "<script src=/panel.js></script></head><body>";

/* v4 hook still in ShellCore (forces NEO regardless of this manager's hook). */
static int v4_active(const shell_t *s) { return s->pid > 0 && s->frame && v4_state(s->pid, s->frame) == V4_PRESENT; }

static void page_header(sbuf *b, const char *active) {
    uint32_t fw_raw = fw_raw_version();
    sb_printf(b, "%s<header><div class=brand><div class=mark>N</div><h1>NeoEnabler <small>FW %x.%02x</small></h1></div>"
              "<nav aria-label='Pages'><a href=/%s>Games</a>%s%s%s</nav>"
              "<form method=post><button class=danger formaction=/quit>Quit</button></form></header>",
              page_head, (fw_raw >> 24) & 0xff, (fw_raw >> 16) & 0xff,
              !strcmp(active, "games") ? " aria-current=page" : "",
              gpu_available() ? "<a href=/gpu" : "", gpu_available() && !strcmp(active, "gpu") ? " aria-current=page" : "",
              gpu_available() ? ">GPU clock</a>" : "");
}

/* Status, game patches update, logs and the advanced overrides. In the legacy panel the forms carry legacy=1, so the
   answer is the legacy page again. */
static void page_status(sbuf *b, int legacy) {
    int mem_ok = memory_available();
    const char *hid = legacy ? "<input type=hidden name=legacy value=1>" : "";
    shell_t s; int st = hook_state(&s);
    if (st == HOOK_OFF && v4_active(&s)) st = HOOK_ON;
    mb_snapshot memory; memory_check(&memory);
    char running[1024]; games_text(running, sizeof(running), 1);
    char patches_esc[768]; html_escape(patches_message, patches_esc, sizeof(patches_esc));
    sb_printf(b, "<aside>"
              "<div class=card><h2>Status</h2>"
              "<div>PS4 Pro mode <span class='state %s'>%s</span></div>"
              "<div style='margin-top:6px'>8GB Mode <span class='state %s'>%s</span>%s</div>"
              "<p><small>Pro mode and 8GB Mode follow the enabled patches of each game. 8GB Mode returns to the original "
              "memory when the game closes.</small></p><b>Game running</b><ul>%s</ul></div>"
              "<div class=card><h2>Game patches</h2><p>%s</p><small>Downloads the patches from the NeoEnabler repository "
              "into " PS_DIR ". Changed files are backed up to " PS_BACKUP_ROOT " first.</small>"
              "<form method=post>%s<button formaction=/patches/update>Update Patches</button></form></div>"
              "<div class=card><h2>Logs</h2><small>Kernel logs, including every NeoEnabler step.</small>"
              "<form method=post>%s<a href=/logs target=_blank><button type=button>Show Logs</button></a>"
              "<a href=/logs/download><button type=button>Download Logs</button></a>"
              "<button formaction=/logs/save>Save Logs</button><button formaction=/diagnostic/save>Save Diagnostic</button></form></div>"
              "<details class=card><summary>Advanced</summary>"
              "<form method=post>%s<input type=hidden name=on value=%d><b>Pro mode for every PS4 game</b> "
              "<span class='state %s'>%s</span><br><small>Ignores the patch selection.</small><br>"
              "<button formaction=/advanced/neo>%s</button></form>"
              "<form method=post style='margin-top:12px'>%s<input type=hidden name=on value=%d><b>Keep 8GB Mode on</b> "
              "<span class='state %s'>%s</span><br><small>For every Pro mode launch, until turned off.</small><br>"
              "<button formaction=/advanced/memory%s>%s</button></form></details>"
              "<small style='text-align:center'>" VERSION "</small></aside>",
              st == HOOK_ON && running_pro == 1 ? "on" : "off",
              st != HOOK_ON ? state_text(st) : running_pro == 1 ? "enabled" : running_pro == 0 ? "disabled" : "ready",
              memory.state == MB_8GIB ? "on" : "off",
              memory.state == MB_8GIB ? "enabled" : !fw_raw_version() ? "firmware unavailable" : !memory_supported() ? "not supported" : !mem_ok ? "unavailable" : "ready",
              memory_community_report() ? "<br><small>Firmware 13.60: Bloodborne 4K with 8GB Mode reported working; more test reports welcome</small>" : "",
              running, patches_esc, hid, hid,
              hid, !force_neo, force_neo ? "on" : "off", force_neo ? "on" : "off", force_neo ? "Turn off" : "Turn on",
              hid, !force_8gib, force_8gib && mem_ok ? "on" : "off",
              !force_8gib ? "off" : mem_ok ? "on" : "requested, not available", !force_8gib && !mem_ok ? " disabled" : "",
              force_8gib ? "Turn off" : "Turn on");
}

/* Set for a request of the legacy panel (its paths, or legacy=1 in the form): pages answer with the legacy panel. */
static int legacy_request;
static void serve_legacy(int fd, const char *msg, const char *q);

/* URL-decoded value of key in an x-www-form-urlencoded body; "" when missing. */
static void form_text(const char *body, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key), o = 0;
    out[0] = 0;
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        for (const char *v = p + kl + 1; *v && *v != '&' && o + 1 < cap; v++) {
            unsigned x;
            if (*v == '+') out[o++] = ' ';
            else if (*v == '%' && v[1] && v[2] && sscanf(v + 1, "%2x", &x) == 1) { out[o++] = (char)x; v += 2; }
            else out[o++] = *v;
        }
        out[o] = 0;
        return;
    }
}

/* Every word of q is in text, ignoring ASCII case. */
static int words_match(const char *text, const char *q) {
    char word[64];
    for (const char *p = q; *p;) {
        while (*p == ' ') p++;
        size_t n = strcspn(p, " ");
        if (!n) break;
        snprintf(word, sizeof word, "%.*s", (int)(n < sizeof word ? n : sizeof word - 1), p);
        size_t wl = strlen(word), tl = strlen(text); int found = 0;
        for (size_t i = 0; !found && i + wl <= tl; i++) found = !strncasecmp(text + i, word, wl);
        if (!found) return 0;
        p += n;
    }
    return 1;
}

/* Panel for the browser of older firmwares: plain pages and forms, no script needed (a patch checkbox submits its
   form), no CSS variables, grid or overlays. */
static const char legacy_head[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NeoEnabler</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#080b12;color:#f4f7fb;font:15px/1.45 sans-serif}"
    "header{padding:12px 16px;background:#111722;border-bottom:1px solid #263145}h1{font-size:19px;margin:0 0 8px}"
    "h1 small{color:#95a2b8;font-weight:normal;font-size:13px;margin-left:8px}a{color:#55a7ff}"
    "main{padding:16px}h2{font-size:18px;margin:0}h2 span{color:#95a2b8;font-weight:normal;font-size:14px}"
    "button{font:inherit;color:#f4f7fb;background:#171f2d;border:1px solid #263145;border-radius:10px;padding:7px 13px;"
    "margin:8px 8px 0 0;cursor:pointer}button.primary{background:#147ee8;border-color:#147ee8}button.danger{background:#3a1f1d;border-color:#5c2c28}"
    "input[type=text]{font:inherit;background:#0c111a;color:#f4f7fb;border:1px solid #263145;border-radius:10px;padding:8px 12px;width:60%}"
    ".msg{margin:0 0 14px;padding:10px 14px;border:1px solid #263145;border-radius:10px;background:#111722}"
    ".game{display:inline-block;vertical-align:top;width:190px;margin:0 12px 12px 0;background:#111722;border:1px solid #263145;"
    "border-radius:14px;overflow:hidden}.game img{display:block;width:190px;height:190px;background:#171f2d}"
    ".info{padding:10px 12px 0}.name{display:block;font-weight:bold}.meta{color:#95a2b8;font-size:13px}"
    ".actions{padding:0 12px 12px}.actions form,.actions a{display:inline}"
    ".card{background:#111722;border:1px solid #263145;border-radius:14px;padding:16px;margin:0 0 14px}"
    "aside{display:block;max-width:640px;margin-top:10px}aside h2{font-size:15px;margin:0 0 8px}"
    ".whead img{width:56px;height:56px;border-radius:10px;vertical-align:middle;margin-right:12px;background:#171f2d}"
    ".whead h2{display:inline;vertical-align:middle}"
    ".facts{margin:14px 0;padding:10px 12px;border:1px solid #263145;border-radius:10px;background:#171f2d}"
    ".facts div{display:inline-block;margin-right:28px}.facts span{display:block;color:#95a2b8;font-size:12px}"
    ".patch{margin:0 0 8px;padding:10px 12px;border:1px solid #263145;border-radius:10px;background:#171f2d}"
    ".patch.on{border-color:#55a7ff}.patch label{display:block;cursor:pointer}.patch input{margin:0 10px 0 0}"
    ".patch i{display:block;color:#95a2b8;font-size:13px;margin:2px 0 0 28px}"
    ".tag{display:inline-block;margin-left:8px;padding:1px 8px;border-radius:10px;font-size:11px;font-weight:bold;"
    "color:#f2bd5d;border:1px solid #6b5a33}"
    ".state{display:inline-block;padding:2px 10px;border-radius:10px;font-size:12px;font-weight:bold;background:#171f2d}"
    ".state.on{background:#173a2d;color:#52d39a}.state.off{color:#95a2b8}"
    "details summary{cursor:pointer;color:#95a2b8;font-size:13px}ul{margin:6px 0 0;padding-left:18px}small{color:#95a2b8}"
    "</style></head><body>";

static void legacy_header(sbuf *b, const char *msg) {
    uint32_t fw_raw = fw_raw_version();
    sb_printf(b, "%s<header><h1>NeoEnabler <small>FW %x.%02x</small></h1><a href=/legacy><button type=button>Games</button></a>"
              "<form method=post action=/quit style='display:inline'><button class=danger>Quit</button></form></header><main>",
              legacy_head, (fw_raw >> 24) & 0xff, (fw_raw >> 16) & 0xff);
    if (msg && msg[0]) { sb_printf(b, "<p class=msg>"); sb_esc(b, msg); sb_printf(b, "</p>"); }
}

/* Legacy games page: search, one card per game (icon, name, title ID, Play, Patches), then the status. */
static void serve_legacy(int fd, const char *msg, const char *q) {
    sbuf b = {0};
    legacy_header(&b, msg);
    int n, shown = 0; game_t *g = games_scan(&n);
    if (n) {
        sb_printf(&b, "<form method=post action=/legacy><input type=text name=q value='");
        sb_esc(&b, q);
        sb_printf(&b, "' placeholder='Search by name or title ID'> <button>Search</button></form><div style='margin-top:14px'>");
    } else sb_printf(&b, "<div class=card><b>No installed game with NeoEnabler patches</b><p><small>Games appear here when "
                         PS_DIR " has a patch for their installed version that needs PS4 Pro mode. Use Update Patches "
                         "to download the patches.</small></p></div><div>");
    for (int i = 0; i < n; i++) {
        char text[160];
        snprintf(text, sizeof text, "%s %s", g[i].name, g[i].tid);
        if (!words_match(text, q)) continue;
        shown++;
        sb_printf(&b, "<div class=game><img src='/icon/%s' alt=''><div class=info><span class=name>", g[i].tid);
        sb_esc(&b, g[i].name[0] ? g[i].name : g[i].tid);
        sb_printf(&b, "</span><span class=meta>%s</span></div><div class=actions><form method=post action=/legacy/play>"
                      "<input type=hidden name=tid value=%s><button class=primary>Play</button></form>"
                      "<a href='/legacy/patches?tid=%s'><button type=button>Patches</button></a></div></div>", g[i].tid, g[i].tid, g[i].tid);
    }
    if (n && !shown) sb_printf(&b, "<p class=card>No game matches the search.</p>");
    sb_printf(&b, "</div>");
    games_free(g, n);
    page_status(&b, 1);
    sb_printf(&b, "</main></body></html>");
    if (b.p) reply_bytes(fd, "200 OK", "text/html; charset=utf-8", "no-store", b.p, b.n);
    else reply(fd, "500 Internal Server Error", "text/plain", "out of memory\n");
    free(b.p);
}

/* Legacy patches page of one game: icon, name and title ID, versions and patch count, search, and the patches. Each
   patch is a form; its checkbox submits it and the answer is this page with the result of game_set_patch. */
static void serve_legacy_patches(int fd, const char *tid, const char *q, const char *msg) {
    int n; game_t *g = games_scan(&n), *x = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(g[i].tid, tid)) x = &g[i];
    if (!x) { games_free(g, n); serve_legacy(fd, "This game has no NeoEnabler patches", ""); return; }
    sbuf b = {0};
    legacy_header(&b, msg);
    size_t sl; char *settings = settings_read(&sl);
    int mem_ok = memory_available(), shown = 0;
    const char *title = x->name[0] ? x->name : x->tid;
    sb_printf(&b, "<div class=whead><img src='/icon/%s' alt=''><h2>", x->tid);
    sb_esc(&b, title);
    sb_printf(&b, " <span>(%s)</span></h2></div><div class=facts><div><span>Game version</span><b>%s</b></div>"
                  "<div><span>Update version</span><b>%s</b></div><div><span>Patches</span><b>%d</b></div></div>"
                  "<form method=post action=/legacy/patches><input type=hidden name=tid value=%s><input type=text name=q value='",
              x->tid, x->game_ver[0] ? x->game_ver : "Unknown", x->update ? (x->update_ver[0] ? x->update_ver : "Unknown") : "None",
              x->count, x->tid);
    sb_esc(&b, q);
    sb_printf(&b, "' placeholder='Search patches'> <button>Search</button></form><div style='margin-top:14px'>");
    for (int k = 0; k < x->count; k++) {
        const gp_patch *p = &x->patches[k];
        char text[2048];
        snprintf(text, sizeof text, "%s %s", p->name, p->note);
        if (!words_match(text, q)) continue;
        shown++;
        int on = gp_setting(settings, sl, p->hash) == 1, blocked = p->needs_8gib && !mem_ok && !on;
        sb_printf(&b, "<form method=post action=/legacy/patches/set class='patch%s'><input type=hidden name=tid value=%s>"
                      "<input type=hidden name=hash value=%s><input type=hidden name=on value=%d><input type=hidden name=q value='",
                  on ? " on" : "", x->tid, p->hash, !on);
        sb_esc(&b, q);
        sb_printf(&b, "'><label><input type=checkbox onclick='this.form.submit()'%s%s><b>", on ? " checked" : "", blocked ? " disabled" : "");
        sb_esc(&b, p->name);
        sb_printf(&b, "</b>%s", p->needs_8gib ? "<span class=tag>8GB Mode</span>" : "");
        int main_i = gp_main_entry(x->patches, x->count, k);
        if (main_i >= 0) { sb_printf(&b, "<span class=tag>Needs a "); sb_esc(&b, x->patches[main_i].title); sb_printf(&b, " resolution</span>"); }
        if (p->note[0]) { sb_printf(&b, "<i>"); sb_esc(&b, p->note); sb_printf(&b, "</i>"); }
        sb_printf(&b, "</label></form>");
    }
    if (!shown) sb_printf(&b, "<p class=card>No patch matches the search.</p>");
    sb_printf(&b, "</div></main></body></html>");
    free(settings); games_free(g, n);
    if (b.p) reply_bytes(fd, "200 OK", "text/html; charset=utf-8", "no-store", b.p, b.n);
    else reply(fd, "500 Internal Server Error", "text/plain", "out of memory\n");
    free(b.p);
}

/* Games with NeoEnabler patches on the left (icon, name, title ID, version, every patch of that version with a
   checkbox); status, patch update, logs and the advanced overrides on the right. */
static void serve_page(int fd, const char *msg) {
    if (legacy_request) { serve_legacy(fd, msg, ""); return; }
    sbuf b = {0};
    page_header(&b, "games");
    sb_printf(&b, "<main>");
    if (msg) { sb_printf(&b, "<p class=msg>"); sb_esc(&b, msg); sb_printf(&b, "</p>"); }
    sb_printf(&b, "<div class=layout><section aria-label='Games'>");
    int n; game_t *g = games_scan(&n);
    size_t sl; char *settings = settings_read(&sl);
    int mem_ok = memory_available();
    if (n) sb_printf(&b, "<input type=search id=game-search class=search placeholder='Search by name or title ID' "
                         "autocomplete=off aria-label='Search games'><p id=no-match class=card hidden>No game matches the search.</p>");
    if (!n) sb_printf(&b, "<div class=card><b>No installed game with NeoEnabler patches</b><p><small>Games appear here when "
                          PS_DIR " has a patch for their installed version that needs PS4 Pro mode. Use Update Patches "
                          "to download the patches.</small></p></div>");
    /* cards (icon, name, title ID, Play and Patches), then one patch window per game, shown by Patches */
    if (n) sb_printf(&b, "<div class=games>");
    for (int i = 0; i < n; i++) {
        const char *title = g[i].name[0] ? g[i].name : g[i].tid;
        sb_printf(&b, "<article class=game data-search='");
        sb_esc(&b, g[i].name);
        sb_printf(&b, " %s'><img class=art src='/icon/%s' alt='' loading=lazy><div class=info><b class=name>", g[i].tid, g[i].tid);
        sb_esc(&b, title);
        sb_printf(&b, "</b><span class=meta>%s</span></div><div class=actions><button type=button class=primary data-play=%s>Play</button>"
                      "<button type=button data-open=w-%s>Patches</button></div></article>", g[i].tid, g[i].tid, g[i].tid);
    }
    if (n) sb_printf(&b, "</div>");
    for (int i = 0; i < n; i++) {
        const char *title = g[i].name[0] ? g[i].name : g[i].tid;
        sb_printf(&b, "<div class=window id=w-%s hidden><div class=sheet role=dialog aria-modal=true aria-label='Patches of ", g[i].tid);
        sb_esc(&b, title);
        sb_printf(&b, "'><div class=whead><img src='/icon/%s' alt='' loading=lazy><h2>", g[i].tid);
        sb_esc(&b, title);
        sb_printf(&b, " <span>(%s)</span></h2><button type=button class=close data-close aria-label=Close>Close</button></div>"
                      "<dl class=facts><div><dt>Game version</dt><dd>%s</dd></div><div><dt>Update version</dt><dd>%s</dd></div>"
                      "<div><dt>Patches</dt><dd>%d</dd></div></dl>"
                      "<input type=search class='search patch-search' placeholder='Search patches' autocomplete=off aria-label='Search patches'>"
                      "<p class='card none' hidden>No patch matches the search.</p><ul class=patches>", g[i].tid,
                  g[i].game_ver[0] ? g[i].game_ver : "Unknown", g[i].update ? (g[i].update_ver[0] ? g[i].update_ver : "Unknown") : "None",
                  g[i].count);
        for (int k = 0; k < g[i].count; k++) {
            const gp_patch *p = &g[i].patches[k];
            int on = gp_setting(settings, sl, p->hash) == 1, blocked = p->needs_8gib && !mem_ok && !on;
            sb_printf(&b, "<li data-search='");
            sb_esc(&b, p->name);
            sb_printf(&b, " ");
            sb_esc(&b, p->note);
            sb_printf(&b, "'><label class=patch><input type=checkbox data-tid=%s data-hash=%s", g[i].tid, p->hash);
            if (p->resolution) { sb_printf(&b, " data-res='"); sb_esc(&b, p->elf); sb_printf(&b, "'"); }
            sb_printf(&b, "%s%s><span><b>", on ? " checked" : "", blocked ? " disabled" : "");
            sb_esc(&b, p->name);
            sb_printf(&b, "</b>%s", p->needs_8gib ? "<span class=tag>8GB Mode</span>" : "");
            int main_i = gp_main_entry(g[i].patches, g[i].count, k);
            if (main_i >= 0) { sb_printf(&b, "<span class=tag>Needs a "); sb_esc(&b, g[i].patches[main_i].title); sb_printf(&b, " resolution</span>"); }
            if (p->note[0]) { sb_printf(&b, "<i>"); sb_esc(&b, p->note); sb_printf(&b, "</i>"); }
            sb_printf(&b, "</span></label></li>");
        }
        sb_printf(&b, "</ul></div></div>");
    }
    free(settings); games_free(g, n);

    sb_printf(&b, "</section>");
    page_status(&b, 0);
    sb_printf(&b, "</div></main></body></html>");
    if (b.p) reply_bytes(fd, "200 OK", "text/html; charset=utf-8", "no-store", b.p, b.n);
    else reply(fd, "500 Internal Server Error", "text/plain", "out of memory\n");
    free(b.p);
}

static void serve_gpu_status(int fd) {
    gpu_refresh(); char current[24] = "null", previous[24] = "null", detail[384], body[1024];
    size_t at = 0;
    for (const unsigned char *p = (const unsigned char *)gpu.message; *p && at + 3 < sizeof detail; p++) {
        if (*p == '"' || *p == '\\') detail[at++] = '\\';
        detail[at++] = *p < 32 ? ' ' : (char)*p;
    }
    detail[at] = 0;
    if (gpu.reading_valid) snprintf(current, sizeof current, "%u", gpu.actual);
    if (gpu.baseline_valid) snprintf(previous, sizeof previous, "%u", gpu.baseline);
    snprintf(body, sizeof body,
        "{\"supported\":%s,\"enabled\":%s,\"applied\":%s,\"state\":\"%s\","
        "\"current_mhz\":%s,\"target_mhz\":%u,\"previous_mhz\":%s,\"max_mhz\":%u,\"detail\":\"%s\"}",
        gpu_backend_supported() ? "true" : "false", gpu.enabled ? "true" : "false",
        gpu.applied ? "true" : "false", gpu_state_text(), current, gpu.target, previous, GPU_MAX_MHZ, detail);
    reply(fd, "200 OK", "application/json", body);
}

static void serve_gpu_page(int fd, const char *msg) {
    gpu_refresh();
    char current[32] = "Unavailable", previous[32] = "Not captured", fw[16];
    uint32_t raw = fw_raw_version();
    snprintf(fw, sizeof fw, "%x.%02x", (raw >> 24) & 255, (raw >> 16) & 255);
    if (gpu.reading_valid) snprintf(current, sizeof current, "%u MHz", gpu.actual);
    if (gpu.baseline_valid) snprintf(previous, sizeof previous, "%u MHz", gpu.baseline);
    const char *dis = gpu_backend_supported() ? "" : " disabled";
    sbuf hb = {0};
    page_header(&hb, "gpu");
    static char body[16384];
    snprintf(body, sizeof body,
        "%s<main>"
        "%s%s%s<div class=grid>"
        "<div class=card><b>Current GPU clock</b><div class=metric id=gpu-current>%s</div>"
        "<small>Live SMU reading. Updates every two seconds.</small></div>"
        "<div class=card><b>Clock boost</b> <span class='state %s' id=gpu-state>%s</span>"
        "<p id=gpu-detail>%s</p><small>Previous clock: <span id=gpu-previous>%s</span></small></div>"
        "<div class='card wide'><form method=post><label for=gpu-target><b>Target GPU clock</b></label>"
        "<p><output id=gpu-value for=gpu-target>%u</output> MHz</p>"
        "<input type=range id=gpu-target name=mhz min=%u max=%u step=50 value=%u%s>"
        "<div><button formaction=/gpu/enable%s>Apply boost</button>"
        "<button formaction=/gpu/disable%s>Disable boost</button></div></form>"
        "<p class=notice>Maximum allowed target: 2200 MHz. Short gameplay testing has passed. "
        "Actual frequency may be lower. Power and thermal controls remain active.</p>"
        "<small>Applies to PS4 games in base or NEO mode. A saved target is requested on the next PS4 launch. "
        "Disable restores the clock captured for the running game. Use Save Settings to keep your choice.</small>"
        "<form method=post><input type=hidden name=view value=gpu><button formaction=/settings/save>Save Settings</button></form>"
        "%s</div></div><p><small>GPU boost is independent of NEO and 8GB Mode. Quitting restores the captured GPU clock.<br>"
        VERSION "</small></p></main>"
        "<script>(function(){const r=document.getElementById('gpu-target'),o=document.getElementById('gpu-value');"
        "r.addEventListener('input',()=>o.textContent=r.value);"
        "async function refresh(){try{const x=await fetch('/gpu/status',{cache:'no-store'});if(!x.ok)return;const s=await x.json();"
        "document.getElementById('gpu-current').textContent=s.current_mhz===null?'Unavailable':s.current_mhz+' MHz';"
        "const b=document.getElementById('gpu-state');b.textContent=s.state;"
        "b.className='state '+(s.applied&&s.state==='enabled'?'on':s.state==='disabled'?'off':'unk');"
        "document.getElementById('gpu-previous').textContent=s.previous_mhz===null?'Not captured':s.previous_mhz+' MHz';"
        "document.getElementById('gpu-detail').textContent=s.detail;"
        "}catch(e){document.getElementById('gpu-current').textContent='Disconnected';}}window.neoGpuTimer=setInterval(refresh,2000);})();</script>"
        "</body></html>",
        hb.p ? hb.p : "", msg ? "<p class=msg>" : "", msg ? msg : "", msg ? "</p>" : "", current,
        gpu.applied && !gpu.blocked ? "on" : gpu.enabled ? "unk" : "off", gpu_state_text(), gpu.message, previous,
        gpu.target, GPU_MIN_MHZ, GPU_MAX_MHZ, gpu.target, dis, dis, dis,
        gpu_backend_supported() ? "" : "<p>GPU clock control is unavailable.</p>");
    free(hb.p);
    reply(fd, "200 OK", "text/html; charset=utf-8", body);
}

#define DIAG_PATH "/data/neo-diagnostic.jsonl"

static size_t generate_diagnostic(char *buf, size_t cap) {
    size_t len = 0;
    unsigned records = 0;
    uint32_t fw = 0; size_t fw_size = 4; int fw_rc, fw_error;
    errno = 0; fw_rc = sysctlbyname("machdep.upd_version", &fw, &fw_size, NULL, 0); fw_error = errno;
    records++;
    len += snprintf(buf + len, cap - len,
        "{\"format\":\"neo-diagnostic-v1\",\"record\":\"session\","
        "\"firmware_raw\":\"0x%08x\",\"firmware_rc\":%d,\"firmware_errno\":%d,\"firmware_bytes\":%zu,"
        "\"sdk_raw\":\"0x%08x\",\"kernel_data_base\":\"0x%016lx\","
        "\"scope\":\"metadata_only\",\"patches\":false,\"snapshot_atomic\":false}\n",
        fw, fw_rc, fw_error, fw_size, kernel_get_fw_version(), (uint64_t)KERNEL_ADDRESS_DATA_BASE);

    /* Self process */
    records++;
    len += snprintf(buf + len, cap - len,
        "{\"format\":\"neo-diagnostic-v1\",\"record\":\"process\",\"pid\":%d,\"kind\":\"native_payload\","
        "\"title_id\":\"\",\"app_info_rc\":-1,\"app_info_errno\":0,"
        "\"neo_driver_rc\":-1,\"base_driver_rc\":-1,\"compat_driver_rc\":-1,\"mode\":\"native\"}\n",
        (int)getpid());

    /* Enumerate games */
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; size_t n = 0;
    unsigned game_count = 0; int complete = 0, scan_rc = -1, scan_error = 0;
    errno = 0; scan_rc = sysctl(mib, 4, NULL, &n, NULL, 0); scan_error = errno;
    if (!scan_rc && n && n <= 32 * 1024 * 1024 - 0x10000) {
        n += 0x10000; unsigned char *b = malloc(n);
        if (b) {
            errno = 0; scan_rc = sysctl(mib, 4, b, &n, NULL, 0); scan_error = errno;
            if (!scan_rc) {
                size_t off = 0;
                while (off + sizeof(struct kinfo_proc) <= n) {
                    struct kinfo_proc *k = (void *)(b + off);
                    if (k->ki_structsize < (int)sizeof(*k) || (size_t)k->ki_structsize > n - off) break;
                    if (!strcmp(k->ki_comm, "eboot.bin") && game_count < 16 && len + 1024 < cap) {
                        app_info_t app = {0}; errno = 0;
                        int app_rc = sceKernelGetAppInfo(k->ki_pid, &app); int app_error = errno;
                        char title[11] = {0}; if (!app_rc) memcpy(title, app.title_id, 10);
                        uint32_t h = 0;
                        int neo = kernel_dynlib_handle(k->ki_pid, "libSceGnmDriverForNeoMode.sprx", &h);
                        int base = kernel_dynlib_handle(k->ki_pid, "libSceGnmDriver.sprx", &h);
                        int compat = kernel_dynlib_handle(k->ki_pid, "libSceGnmDriverCompat1.sprx", &h);
                        const char *mode = !neo ? "neo" : !base ? "base" : !compat ? "compat_unknown" : "unknown";
                        records++;
                        len += snprintf(buf + len, cap - len,
                            "{\"format\":\"neo-diagnostic-v1\",\"record\":\"process\",\"pid\":%d,\"kind\":\"game\","
                            "\"title_id\":\"%s\",\"app_info_rc\":%d,\"app_info_errno\":%d,"
                            "\"neo_driver_rc\":%d,\"base_driver_rc\":%d,\"compat_driver_rc\":%d,\"mode\":\"%s\"}\n",
                            (int)k->ki_pid, title, app_rc, app_error, neo, base, compat, mode);
                        /* DMEM/FMEM queries */
                        static const char *nodes[] = {"kern.app.dmem_size_pid", "kern.app.fmem_size_pid", "kern.app.fmem_avail_pid"};
                        for (int q = 0; q < 3 && len + 512 < cap; q++) {
                            int qmib[24]; size_t qc = 23, qsz = 8; uint64_t qval = 0;
                            errno = 0; int qrc = sysctlnametomib(nodes[q], qmib, &qc);
                            if (!qrc && qc <= 23) { qmib[qc++] = k->ki_pid; qrc = sysctl(qmib, (unsigned)qc, &qval, &qsz, NULL, 0); }
                            else qrc = -1;
                            int qerr = errno;
                            records++;
                            if (!qrc && qsz == 8)
                                len += snprintf(buf + len, cap - len,
                                    "{\"format\":\"neo-diagnostic-v1\",\"record\":\"query\",\"pid\":%d,\"node\":\"%s\","
                                    "\"rc\":%d,\"errno\":%d,\"returned_bytes\":%zu,\"value_hex\":\"0x%016lx\"}\n",
                                    (int)k->ki_pid, nodes[q], qrc, qerr, qsz, qval);
                            else
                                len += snprintf(buf + len, cap - len,
                                    "{\"format\":\"neo-diagnostic-v1\",\"record\":\"query\",\"pid\":%d,\"node\":\"%s\","
                                    "\"rc\":%d,\"errno\":%d,\"returned_bytes\":%zu,\"value_hex\":null}\n",
                                    (int)k->ki_pid, nodes[q], qrc, qerr, qsz);
                        }
                        game_count++;
                    }
                    off += (size_t)k->ki_structsize;
                }
                complete = off == n;
            }
            free(b);
        } else { scan_rc = -1; scan_error = ENOMEM; }
    } else if (!scan_rc) { scan_rc = -1; scan_error = EOVERFLOW; }

    len += snprintf(buf + len, cap - len,
        "{\"format\":\"neo-diagnostic-v1\",\"record\":\"end\",\"records_before_end\":%u,"
        "\"scan_complete\":%s,\"game_count\":%u,\"scan_rc\":%d,\"scan_errno\":%d}\n",
        records, complete ? "true" : "false", game_count, scan_rc, scan_error);

    return len;
}

static void serve_diagnostic(int fd) {
    static char buf[16384];
    generate_diagnostic(buf, sizeof(buf));
    reply(fd, "200 OK", "application/x-ndjson; charset=utf-8", buf);
}

static void serve_diagnostic_save(int fd) {
    static char buf[16384];
    generate_diagnostic(buf, sizeof(buf));
    int ok = !write_file(DIAG_PATH, buf, strlen(buf));
    char msg[256];
    snprintf(msg, sizeof(msg), ok ? "Diagnostic saved to " DIAG_PATH : "Failed to write " DIAG_PATH);
    notify(msg);
    serve_page(fd, msg);
}

static int do_set(int enable, char *msg, size_t cap, int show) {
    int rc = hook_set(enable);
    shell_t s; int st = hook_state(&s);
    if (!enable && !rc && (s.pid <= 0 || !s.frame || v4_remove(s.pid, s.frame))) rc = 9;
    if (!rc) snprintf(msg, cap, "NeoEnabler %s", enable ? "enabled" : "disabled");
    else snprintf(msg, cap, "NeoEnabler: failed to %s (rc=%d, state: %s)", enable ? "enable" : "disable", rc, state_text(st));
    if (show) notify(msg); else log_line("%s", msg);
    return rc;
}

/* State after an action: NEO hook, legacy v4 hook, actual 8 GiB budget and the requested one. */
static void log_state(void) {
    shell_t s; int st = hook_state(&s);
    if (st == HOOK_BAD_CAVE) { char d[400]; bad_cave_text(&s, d, sizeof(d)); log_line("%s", d); }
    mb_snapshot m; memory_check(&m);
    log_line("state: NEO hook %s, v4 hook %s, 8GB Mode %s (requested %s): %s", state_text(st),
             v4_active(&s) ? "present" : "absent",
             m.state == MB_8GIB ? "enabled" : m.state == MB_UNKNOWN ? "unavailable" : "disabled",
             memory_target ? "on" : "off", m.detail);
    log_line("state: GPU %s, requested %u MHz, SMU %s%u MHz, owner PID %d: %s",
             gpu_state_text(), gpu.target, gpu.reading_valid ? "" : "unavailable/last=", gpu.actual, gpu.owner_pid, gpu.message);
}

/* Identity of the eboot.bin of every game seen mounted, so a report shows whether two title IDs run the same binary
   (the patches are bytes at fixed addresses). The game's PFS is mounted under /mnt/sandbox/pfsmnt while it runs. The
   file is read 1 MiB per main loop pass so launch questions and the panel are not held up. fnv64 is FNV-1a over the
   whole file: it tells binaries apart, it is not a cryptographic hash. */
#define BIN_MAX 24
#define PFSMNT "/mnt/sandbox/pfsmnt"
static struct { char tid[10]; unsigned long long size, fnv; char from[24]; } bins[BIN_MAX];
static int bin_count;
static struct { int fd; char tid[10]; unsigned long long size, fnv; char from[24]; } binjob = {.fd = -1};

static int bin_known(const char *tid) {
    for (int i = 0; i < bin_count; i++) if (!strcmp(bins[i].tid, tid)) return 1;
    return 0;
}

static void binary_scan(void) {
    DIR *d = opendir(PFSMNT);
    if (!d) return;
    struct dirent *e;
    while (binjob.fd < 0 && (e = readdir(d))) {
        if (strlen(e->d_name) != 14 || strcmp(e->d_name + 9, "-app0")) continue;
        char tid[10]; snprintf(tid, sizeof tid, "%.9s", e->d_name);
        if (!game_tid(tid) || bin_known(tid) || bin_count >= BIN_MAX) continue;
        static const char *const dirs[] = {"-app0-patch0-union", "-app0"};
        for (int k = 0; k < 2 && binjob.fd < 0; k++) {
            char path[96]; snprintf(path, sizeof path, PFSMNT "/%s%s/eboot.bin", tid, dirs[k]);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            binjob.fd = fd; binjob.size = 0; binjob.fnv = 1469598103934665603ULL;
            snprintf(binjob.tid, sizeof binjob.tid, "%s", tid); snprintf(binjob.from, sizeof binjob.from, "%s", dirs[k] + 1);
        }
    }
    closedir(d);
}

static void binary_tick(void) {
    static time_t last_scan;
    if (binjob.fd < 0) {
        time_t now = time(NULL);
        if (now == last_scan || now - last_scan < 2) return;
        last_scan = now; binary_scan();
        if (binjob.fd < 0) return;
    }
    static unsigned char buf[1 << 20];
    ssize_t n = read(binjob.fd, buf, sizeof buf);
    if (n > 0) {
        unsigned long long h = binjob.fnv;
        for (ssize_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ULL;
        binjob.fnv = h; binjob.size += (unsigned long long)n;
        return;
    }
    close(binjob.fd); binjob.fd = -1;
    if (n < 0 || !binjob.size) { log_line("binary: %s eboot.bin could not be read (errno %d)", binjob.tid, errno); snprintf(bins[bin_count].tid, 10, "%s", binjob.tid); bins[bin_count].size = 0; bin_count++; return; }
    snprintf(bins[bin_count].tid, 10, "%s", binjob.tid); snprintf(bins[bin_count].from, sizeof bins[0].from, "%s", binjob.from);
    bins[bin_count].size = binjob.size; bins[bin_count].fnv = binjob.fnv; bin_count++;
    log_line("binary: %s eboot.bin from %s size %llu fnv64 %016llx", binjob.tid, binjob.from, binjob.size, binjob.fnv);
}

#define LOGS_PATH "/data/neo-logs.txt"
#define LOGS_MAX (1024 * 1024)

/* Newest LOGS_MAX bytes of kern.msgbuf, starting at a full line, NUL terminated. The ring is 128 KiB on the PS5, but
   the size is asked first so a bigger one is read whole and trimmed instead of failing with ENOMEM. Returns 0 and a
   malloc'd buffer, or -1 with errno set. */
static int read_msgbuf(char **out, size_t *len) {
    size_t need = 0;
    errno = 0;
    if (sysctlbyname("kern.msgbuf", NULL, &need, NULL, 0) || !need || need > 64u * 1024 * 1024) { if (!errno) errno = EFBIG; return -1; }
    size_t cap = need + 4096, n = cap;
    char *buf = malloc(cap + 1);
    if (!buf) return -1;
    if (sysctlbyname("kern.msgbuf", buf, &n, NULL, 0) || !n) { int e = errno ? errno : EIO; free(buf); errno = e; return -1; }
    size_t skip = 0;
    if (n > LOGS_MAX) {
        skip = n - LOGS_MAX;
        char *nl = memchr(buf + skip, '\n', n - skip);
        if (nl && (size_t)(nl - buf) + 1 < n) skip = (size_t)(nl - buf) + 1;
        memmove(buf, buf + skip, n - skip);
        n -= skip;
    }
    buf[n] = '\0';
    *out = buf; *len = n;
    return 0;
}

static void sb_raw(sbuf *b, const void *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 16384;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) return;
        b->p = q; b->cap = cap;
    }
    memcpy(b->p + b->n, p, n); b->n += n; b->p[b->n] = '\0';
}

/* Polls the kernel log 4 times a second. Every new line goes, without noise and personal ids and with identical
   runs shortened, to kernel.log (3 MiB, then kernel.log.old). The launch answers of the hook and the crash reports
   are also copied to neo.log, where they sit with NeoEnabler's own events. The ring is overwritten in 10 to 25
   seconds when idle and noise is almost all of it, so what is kept lasts much longer. */
/* The current file is closed after 30 minutes (of this run, or from its first line after a restart) or at 4 MiB:
   kernel.log.2 is dropped, .1 becomes .2 and the current file becomes .1. */
static long long klog_chunk_ms = -1;

static void klog_append(const char *data, size_t n, long long now_ms) {
    struct stat st;
    if (klog_chunk_ms < 0) klog_chunk_ms = now_ms;
    int full = !stat(KLOG_PATH, &st) && (size_t)st.st_size >= KLOG_MAX;
    if (full || now_ms - klog_chunk_ms >= KLOG_CHUNK_MS) {
        if (!stat(KLOG_PATH, &st)) { unlink(KLOG_OLD2); rename(KLOG_OLD1, KLOG_OLD2); rename(KLOG_PATH, KLOG_OLD1); }
        klog_chunk_ms = now_ms;
    }
    append_log(KLOG_PATH, NULL, (size_t)-1, data, n);
}

static struct { sbuf out; char last[1200]; size_t last_n, same; unsigned long hidden[LM_NOISE_KINDS]; long long note_ms; } km;

static void km_flush_same(void) {
    if (!km.same) return;
    char st[64], note[80];
    size_t k = log_stamp(st, sizeof st);
    sb_printf(&km.out, "%.*s(line above repeated %zu more times)\n", (int)k, st, km.same);
    (void)note; km.same = 0;
}

static void mirror_emit(void *ctx, const char *line, size_t n) {
    (void)ctx;
    if (lm_has(line, n, "[NeoEnabler]")) return;
    int nz = lm_noise_kind(line, n);
    if (nz) { km.hidden[nz - 1]++; return; }
    char red[1400]; size_t rn = lm_redact(line, n < 1200 ? n : 1200, red, sizeof red);
    if (lm_wanted(line, n)) file_log("kernel: ", red, rn);
    if (rn == km.last_n && !memcmp(red, km.last, rn)) { km.same++; return; }
    km_flush_same();
    char st[64]; size_t k = log_stamp(st, sizeof st);
    sb_raw(&km.out, st, k); sb_raw(&km.out, red, rn); sb_raw(&km.out, "\n", 1);
    memcpy(km.last, red, rn); km.last_n = rn;
}

static void log_mirror_tick(void) {
    static lm_state st;
    char *buf; size_t len;
    if (read_msgbuf(&buf, &len)) return;
    km.out.n = 0;
    int gap = lm_scan(&st, buf, len, 1, mirror_emit, NULL);
    free(buf);
    km_flush_same();
    if (gap > 0) {
        char s[64]; size_t k = log_stamp(s, sizeof s);
        sb_printf(&km.out, "%.*s-- lines may have been lost here: the kernel log turned over between two reads --\n", (int)k, s);
        log_line("kernel log: lines may have been lost between two reads");
    }
    /* once a minute: how many noise lines were left out, by kind, so the filter is visible */
    struct timespec mt; clock_gettime(CLOCK_MONOTONIC, &mt);
    long long ms = (long long)mt.tv_sec * 1000 + mt.tv_nsec / 1000000;
    if (ms - km.note_ms >= 60000) {
        km.note_ms = ms;
        char s[64]; size_t k = log_stamp(s, sizeof s); int any = 0;
        for (int i = 0; i < LM_NOISE_KINDS; i++) if (km.hidden[i]) {
            if (!any) sb_printf(&km.out, "%.*s-- lines left out in the last minute:", (int)k, s);
            sb_printf(&km.out, "%s %s %lu", any ? "," : "", lm_noise_names[i], km.hidden[i]);
            any = 1; km.hidden[i] = 0;
        }
        if (any) sb_raw(&km.out, " --\n", 4);
    }
    if (km.out.n) klog_append(km.out.p, km.out.n, ms);
}

static void sb_file(sbuf *b, const char *title, const char *path, size_t max) {
    size_t len = 0; char *data = ps_read(path, max + 4096, &len);
    sb_printf(b, "\n==== %s ====\n", title);
    if (data) { sb_raw(b, data, len); if (len && data[len - 1] != '\n') sb_raw(b, "\n", 1); }
    else sb_printf(b, "(none)\n");
    free(data);
}

/* State of the console, the hook and the games, so a report does not depend on lines the kernel log already lost. */
static void build_report(sbuf *b) {
    shell_t s; int st = hook_state(&s);
    unsigned fw = fw_major();
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm);
    char now[32]; strftime(now, sizeof now, "%Y-%m-%d %H:%M:%S", &tm);
    struct timespec mono; clock_gettime(CLOCK_MONOTONIC, &mono);
    sb_printf(b, "==== Report ====\n%s\nfirmware %x.%02x\nconsole time %s (UTC, may be wrong; each log line also has +seconds since the manager started)\n"
              "manager running since %lds\nhook: %s\n", VERSION, fw >> 8, fw & 0xff, now, (long)(mono.tv_sec - manager_start.tv_sec), state_text(st));
    if (st == HOOK_BAD_CAVE) { char d[400]; bad_cave_text(&s, d, sizeof d); sb_printf(b, "%s\n", d); }
    mb_snapshot m; memory_check(&m);
    sb_printf(b, "8GB Mode: %s (requested %s): %s\n", m.state == MB_8GIB ? "enabled" : m.state == MB_UNKNOWN ? "unavailable" : "disabled",
              memory_target ? "on" : "off", m.detail);
    sb_printf(b, "GPU: %s, requested %u MHz, SMU %s%u MHz: %s\n", gpu_state_text(), gpu.target,
              gpu.reading_valid ? "" : "unavailable/last=", gpu.actual, gpu.message);
    sb_printf(b, "patches: %s\n", patches_message);
    { char first[64] = ""; FILE *kf = fopen(KLOG_OLD2, "r"); if (!kf) kf = fopen(KLOG_OLD1, "r"); if (!kf) kf = fopen(KLOG_PATH, "r");
      if (kf) { if (fgets(first, 24, kf)) first[23] = 0; fclose(kf); }
      sb_printf(b, "kernel log kept since: %s (console time)\n", first[0] ? first : "nothing yet"); }
    { size_t bl = 0; char *bt = ps_read("/data/user/patches/build.txt", 256, &bl);
      sb_printf(b, "ps-patch-system %s", bt && bl ? bt : "patch database version unknown (the payload version is not available)\n");
      if (bt && bl && bt[bl - 1] != '\n') sb_raw(b, "\n", 1);
      free(bt); }
    sb_printf(b, "eboot.bin of games seen mounted since the manager started (fnv64, to compare binaries):%s\n", bin_count ? "" : " none yet");
    for (int i = 0; i < bin_count; i++) {
        if (bins[i].size) sb_printf(b, "  %s from %s size %llu fnv64 %016llx\n", bins[i].tid, bins[i].from, bins[i].size, bins[i].fnv);
        else sb_printf(b, "  %s could not be read\n", bins[i].tid);
    }
    int n; game_t *g = games_scan(&n);
    size_t sl = 0; char *settings = settings_read(&sl);
    for (int i = 0; i < n; i++) {
        sb_printf(b, "game %s %s version %s%s, enabled:", g[i].tid, g[i].name, g[i].version, g[i].update ? " (update installed)" : "");
        int any = 0;
        for (int k = 0; k < g[i].count; k++)
            if (settings && gp_setting(settings, sl, g[i].patches[k].hash) == 1) { sb_printf(b, " [%s]", g[i].patches[k].name); any = 1; }
        sb_printf(b, "%s\n", any ? "" : " none");
    }
    free(settings); games_free(g, n);
}

/* One text for Show, Download and Save: report, NeoEnabler's own log and the kernel log. */
static int build_logs(sbuf *b) {
    build_report(b);
    sb_file(b, "NeoEnabler log (older)", NEO_LOG_OLD, NEO_LOG_MAX);
    sb_file(b, "NeoEnabler log", NEO_LOG_PATH, NEO_LOG_MAX);
    sb_file(b, "Kernel log kept by NeoEnabler, oldest part (noise hidden, IPs, MACs, e-mails and account, device ids removed)", KLOG_OLD2, KLOG_MAX);
    sb_file(b, "Kernel log kept by NeoEnabler, older part", KLOG_OLD1, KLOG_MAX);
    sb_file(b, "Kernel log kept by NeoEnabler, newest part", KLOG_PATH, KLOG_MAX);
    return 0;
}

static void serve_logs_show(int fd) {
    sbuf b = {0};
    build_logs(&b);
    if (!b.p) { reply(fd, "500 Internal Server Error", "text/plain", "out of memory\n"); return; }
    reply_bytes(fd, "200 OK", "text/plain; charset=utf-8", "no-store", b.p, b.n);
    free(b.p);
}

/* Same text as Show Logs, sent as a file so the browser downloads it. */
static void serve_logs_download(int fd) {
    sbuf b = {0};
    build_logs(&b);
    if (!b.p) { reply(fd, "500 Internal Server Error", "text/plain", "out of memory\n"); return; }
    char head[256];
    int h = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Disposition: attachment; filename=\"neo-logs.txt\"\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n", b.n);
    send_all(fd, head, (size_t)h); send_all(fd, b.p, b.n);
    free(b.p);
}

static void serve_logs_save(int fd) {
    sbuf b = {0};
    char msg[256];
    build_logs(&b);
    int ok = b.p && !write_file(LOGS_PATH, b.p, b.n);
    snprintf(msg, sizeof(msg), ok ? "Logs saved to " LOGS_PATH " (%zu bytes)" : "Failed to write " LOGS_PATH, b.n);
    free(b.p);
    notify(msg); serve_page(fd, msg);
}

static void patches_update(void) {
    ps_result r;
    struct timespec a, z; clock_gettime(CLOCK_MONOTONIC, &a);
    patch_sync(&r);
    clock_gettime(CLOCK_MONOTONIC, &z);
    log_line("patches: sync took %ld ms", (long)((z.tv_sec - a.tv_sec) * 1000 + (z.tv_nsec - a.tv_nsec) / 1000000));
    snprintf(patches_message, sizeof(patches_message), "%s", r.message);
    notify(r.message);
}

static void gpu_tick(void) {
    static unsigned previous_actions;
    static int previous_result = 9999;
    int result = gpu_poll(gpu_game_pid());
    if (result != previous_result || gpu.actions != previous_actions) {
        log_line("GPU: %s (result %d)", gpu.message, result);
        previous_actions = gpu.actions; previous_result = result;
    }
}

/* Launch questions. The hook posts the title ID of every PS4 launch and waits up to LAUNCH_WAIT_S seconds; the
   manager answers Pro mode when an enabled patch of that game needs it, after switching the 8 GiB budget to what
   the launch needs (the kernel reads it when it creates the game process). 8 GiB switched on for a launch returns
   to the original budget once the game has closed. */
#define LAUNCH_WAIT_S 5
static shell_t live;
static int live_ok;
static uint32_t last_seq;
static uint64_t tsc_hz = 1600000000ULL;
static struct { int active, needs; uint32_t seq, attr_off; char tid[10]; time_t since; } pending;
static struct { char tid[10]; uint32_t offset; } last_attr;   /* last detected ATTRIBUTE offset, for /status */
static struct { int active, seen_game; char tid[10]; time_t since; } auto8;

static uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

static uint64_t tsc_rate(void) {
    struct timespec a, c;
    clock_gettime(CLOCK_MONOTONIC, &a); uint64_t t0 = rdtsc();
    usleep(100000);
    clock_gettime(CLOCK_MONOTONIC, &c); uint64_t t1 = rdtsc();
    double sec = (double)(c.tv_sec - a.tv_sec) + (double)(c.tv_nsec - a.tv_nsec) / 1e9;
    return sec > 0.05 && t1 > t0 ? (uint64_t)((double)(t1 - t0) / sec) : 1600000000ULL;
}

/* A patch that needs 8 GiB cannot work while 8 GiB is not available (no hypervisor exploit, or an unsupported
   firmware): turns off the enabled ones, of one game or of every game, and returns how many. */
static int patches_drop_8gib(const char *only_tid) {
    if (memory_available()) return 0;
    int dropped = games_drop_8gib(only_tid, memory_message);
    if (dropped) {
        char note[200];
        snprintf(note, sizeof note, "%d patch%s that need 8GB Mode turned off: 8GB Mode is not available", dropped, dropped == 1 ? "" : "es");
        notify(note);
    }
    return dropped;
}

/* A launch that wanted 8 GiB was released without it: its 8 GiB patches are turned off before ps-patch-system reads
   the settings for that game (it does when the game process starts, after the mount). */
static void memory_launch_refused(const char *tid) {
    if (!game_tid(tid)) return;
    int dropped = games_drop_8gib(tid, "8GB Mode was not applied to this launch");
    if (dropped) {
        char note[200];
        snprintf(note, sizeof note, "8GB Mode could not be applied to %s: %d patch%s that need it turned off", tid, dropped, dropped == 1 ? "" : "es");
        notify(note);
    }
}

/* Patches can also be turned on in the ps-patch-system Patch Manager: when its settings file changes, 8 GiB patches
   are turned off again while 8 GiB is not available. */
static void settings_watch(void) {
    static time_t mtime; static off_t size = -1;
    struct stat st;
    if (stat(SETTINGS_XML, &st) || (st.st_mtime == mtime && st.st_size == size)) return;
    mtime = st.st_mtime; size = st.st_size;
    patches_drop_8gib(NULL);
}

/* "Keep 8GB Mode on" is a request for the budget. Without 8 GiB (no hypervisor exploit, or an unsupported firmware) there
   is nothing to request: it is cleared and saved, so it does not stay on and come back at the next unlock. */
static void drop_8gib_request(void) {
    if (!force_8gib || memory_available()) return;
    force_8gib = 0;
    int saved = !settings_save();
    log_line("Keep 8GB Mode on: cleared, 8GB Mode is not available (needs firmware 13.20 or 13.60 with a validated memory launch hook)%s",
             saved ? "" : " (settings not saved)");
}

/* Called after the hook changes and every two seconds: questions are only read from a live hook. */
static void launch_attach(int ask) {
    int was = live_ok;
    live_ok = shim_live(&live);
    if (!live_ok) return;
    if (!was) {
        char tid[10]; uint64_t info;
        if (shim_question(&live, &last_seq, tid, &info)) { live_ok = 0; return; }   /* earlier questions are not ours */
        if (ask < 0) log_line("launch: questions enabled");   /* hook installed after the start */
    }
    if (!was || ask >= 0) shim_configure(&live, force_neo, ask == 0 ? 0 : tsc_hz * LAUNCH_WAIT_S);
}

/* ATTRIBUTE offset in the app info of this launch (0: the shim uses its table offset). Logs the choice, and the
   app info around the known offsets when the table offset is not the one found, so a report shows the layout. */
static uint32_t launch_attr_offset(const char *tid, uint64_t info) {
    static unsigned char buf[AD_INFO_SIZE];
    char path[64]; size_t n = 0;
    uint32_t attr, attr2 = 0;
    snprintf(path, sizeof path, APPMETA_DIR "/%.9s/param.sfo", tid);
    char *sfo = ps_read(path, 64 << 10, &n);
    int has_attr = sfo && !ad_sfo_int((unsigned char *)sfo, n, "ATTRIBUTE", &attr);
    int has_attr2 = sfo && !ad_sfo_int((unsigned char *)sfo, n, "ATTRIBUTE2", &attr2);
    free(sfo);
    if (!has_attr) { log_line("launch: %s: ATTRIBUTE not found in %s, using the table offset 0x%x", tid, path, live.attr_off); return 0; }
    if (!info || kernel_proc_copyout(live.pid, (uintptr_t)info, buf, sizeof buf)) {
        log_line("launch: %s: app info at %llx unreadable, using the table offset 0x%x", tid, (unsigned long long)info, live.attr_off);
        return 0;
    }
    ad_result r; ad_pick(buf, sizeof buf, attr, has_attr2, attr2, live.attr_off, &r);
    char list[AD_MAX_CANDIDATES * 7 + 8] = ""; size_t used = 0;
    for (int i = 0; i < r.count && i < AD_MAX_CANDIDATES; i++)
        used += (size_t)snprintf(list + used, sizeof list - used, "%s0x%x", i ? " " : "", r.candidates[i]);
    log_line("launch: %s: ATTRIBUTE 0x%x%s, matches at [%s]%s, %s: %s 0x%x (table 0x%x)", tid, attr,
             has_attr2 ? "" : " (no ATTRIBUTE2)", list, r.count > AD_MAX_CANDIDATES ? " and more" : "", r.why,
             r.offset ? "using" : "keeping", r.offset ? r.offset : live.attr_off, live.attr_off);
    if (r.offset != live.attr_off) {
        char dump[400];   /* log lines hold 512 characters */
        for (size_t from = 0x1c0; from < 0x2a0; from += 0x70) {
            ad_dump(buf, sizeof buf, from, from + 0x70, dump, sizeof dump);
            log_line("launch: %s: app info %s", tid, dump);
        }
    }
    snprintf(last_attr.tid, sizeof last_attr.tid, "%s", tid);
    last_attr.offset = r.offset;
    return r.offset;
}

/* The hook can only be installed after ps-patch-system has redirected the getAppInfo call. When NeoEnabler starts
   first (payloads sent together or again), the install fails, so every 2 s, while the hook is not live, it is
   installed as soon as the call points to the ps-patch-system hook (or to an older NeoEnabler shim). */
static void hook_retry(void) {
    static int last_rc = -1;
    if (live_ok) return;
    shell_t s; int st = hook_state(&s);
    if (st != HOOK_OFF && st != HOOK_OLD) return;
    int rc = hook_set(1);
    if (rc) {
        if (rc != last_rc) log_line("hook retry: failed to enable (rc=%d, state: %s)", rc, state_text(st));
        last_rc = rc;
        return;
    }
    last_rc = -1;
    notify("NeoEnabler enabled: ps-patch-system finished loading after NeoEnabler started");
    log_state();   /* questions start at the next check: right after the install ShellCore can still be resuming */
}

static void launch_poll(void) {
    if (!live_ok) return;
    if (!pending.active) {
        uint32_t seq; char tid[10]; uint64_t info;
        if (shim_question(&live, &seq, tid, &info) || seq == last_seq) return;
        last_seq = seq;
        pending.active = 1; pending.seq = seq; pending.since = time(NULL);
        snprintf(pending.tid, sizeof pending.tid, "%s", tid);
        pending.attr_off = launch_attr_offset(tid, info);
        patches_drop_8gib(tid);
        pending.needs = game_needs(tid);
        if (force_neo) pending.needs |= NEEDS_PRO;
        log_line("launch: %s needs %s%s", tid, pending.needs & NEEDS_PRO ? "Pro mode" : "base mode",
                 pending.needs & NEEDS_8GIB ? " and 8GB Mode" : "");
    }
    int pro = (pending.needs & NEEDS_PRO) != 0, ok = 1;
    int mem_ok = memory_available();
    int want8 = mem_ok && (force_8gib || (pending.needs & NEEDS_8GIB));
    /* the budget only applies to Pro mode processes */
    if (pro && mem_ok) {
        int rc = memory_request(want8);
        ok = rc == 0;
        if (!ok && time(NULL) - pending.since < LAUNCH_WAIT_S - 1) return;   /* e.g. the previous game still closing */
        if (!ok) {
            mb_snapshot m;   /* keep the budget as it is instead of switching it later, after this game */
            if (!memory_check(&m)) memory_target = m.state == MB_8GIB;
            log_line("launch: %s: memory not switched (%s)", pending.tid, memory_message);
        }
        else if (want8 && !force_8gib) {
            auto8.active = 1; auto8.seen_game = 0; auto8.since = time(NULL);
            snprintf(auto8.tid, sizeof auto8.tid, "%s", pending.tid);
        } else if (!want8) auto8.active = 0;
    }
    shim_answer(&live, pending.seq, (unsigned char)pro, pending.attr_off);
    pending.active = 0;
    log_line("launch: %s starts in %s mode%s", pending.tid, pro ? "Pro" : "base",
             pro && mem_ok && ok ? (want8 ? " with 8GB Mode" : " with the original memory") : "");
}

/* 8 GiB switched on for a launch goes back to the original budget when no game runs any more. */
static void launch_tick(void) {
    if (!auto8.active || force_8gib) return;
    int games = mb_games();
    if (games > 0) { auto8.seen_game = 1; return; }
    if (games < 0 || (!auto8.seen_game && time(NULL) - auto8.since < 60)) return;
    auto8.active = 0;
    memory_request(0);
    log_line("8GB Mode: %s closed: %s", auto8.tid, memory_message);
}

static int memory_launch_wants(const char *tid) {
    if (!game_tid(tid)) return 0;
    int needs = game_needs(tid);
    return force_8gib || (needs & NEEDS_8GIB); /* actual Pro mode is checked in HELD */
}

/* Value of key in an x-www-form-urlencoded body, letters, digits and '.' only. */
static int form_value(const char *body, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        const char *v = p + kl + 1; size_t n = strcspn(v, "&");
        if (!n || n >= cap) return -1;
        for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)v[i]) && v[i] != '.') return -1;
        memcpy(out, v, n); out[n] = 0;
        return 0;
    }
    return -1;
}

static void serve_icon(int fd, const char *tid) {
    char path[96]; size_t n;
    if (!game_tid(tid)) { reply(fd, "404 Not Found", "text/plain", "not found\n"); return; }
    snprintf(path, sizeof path, "/user/appmeta/%s/icon0.png", tid);
    char *png = ps_read(path, 4 << 20, &n);
    if (!png) { reply(fd, "404 Not Found", "text/plain", "not found\n"); return; }
    reply_bytes(fd, "200 OK", "image/png", "max-age=86400", png, n);
    free(png);
}

/* Listening socket of the web panel; -1 when the port cannot be bound. */
static int panel_socket(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 4)) { close(s); return -1; }
    return s;
}

/* 0 when a connection to the panel port on this console is refused: the listening socket is dead (rest mode can leave
   it so while this process keeps running). The probe connection is accepted and answered like any empty request. */
static int panel_alive(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alive = !connect(s, (struct sockaddr *)&addr, sizeof(addr)) || errno != ECONNREFUSED;
    close(s);
    return alive;
}

/* Start time of a running process (KERN_PROC_PROC enumeration, the tested query on this target); 0 when found. */
static int proc_start(pid_t pid, struct timeval *start) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}; size_t n = 0; int found = 0;
    if (sysctl(mib, 4, NULL, &n, NULL, 0) || !n || n > 32 * 1024 * 1024) return -1;
    unsigned char *buf = malloc(n);
    if (!buf || sysctl(mib, 4, buf, &n, NULL, 0)) { free(buf); return -1; }
    for (size_t off = 0; !found && off + sizeof(struct kinfo_proc) <= n;) {
        struct kinfo_proc *ki = (void *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > n - off) break;
        if (ki->ki_pid == pid) { *start = ki->ki_start; found = 1; }
        off += (size_t)ki->ki_structsize;
    }
    free(buf);
    return found ? 0 : 1;
}

/* The previous run can stay alive without a working panel (rest mode), still owning its launch hook. Each run
   records its PID and process start time; a process with both values is that run and nothing else, even after a
   reboot reuses the PID. Returns 1 when the previous run is confirmed gone (exited, quit, or killed here), 0 when
   it is unknown (no record: first run or 0.4.0 and older), and passes on the slot it recorded. */
#define MANAGER_PID_FILE NEO_LOG_DIR "/manager.pid"
static int previous_run_gone(void) {
    int gone = 0; long pid = 0, sec = 0, usec = 0; unsigned long slot = 0;
    FILE *f = fopen(MANAGER_PID_FILE, "r");
    if (f) {
        if (fscanf(f, "%ld %ld %ld %lx", &pid, &sec, &usec, &slot) >= 3 && pid > 0 && pid != getpid()) {
            struct timeval st;
            int r = proc_start((pid_t)pid, &st);
            if (r == 1 || (!r && (st.tv_sec != sec || st.tv_usec != usec))) {
                gone = 1;
                log_line("previous run (PID %ld) has exited", pid);
            } else if (!r && open_game((char[10]){0})) {
                /* it may own an 8GB Mode transaction that it restores when the game closes */
                log_line("previous run (PID %ld) is still running and a game is open; left running", pid);
            } else if (!r) {
                log_line("previous run (PID %ld) is still running without its panel; stopping it", pid);
                if (!kill((pid_t)pid, SIGKILL))
                    for (int i = 0; i < 50 && !gone; i++) {
                        usleep(100000);
                        if (proc_start((pid_t)pid, &st) == 1) gone = 1;
                    }
                log_line("previous run (PID %ld) %s", pid, gone ? "stopped" : "did not stop; its launch hook is left alone");
            }
        }
        fclose(f);
    }
    if (gone) bm_reuse_slot = (uint32_t)slot;
    return gone;
}

/* Records this run (PID, start time, launch-hook slot offset) for the next start; see previous_run_gone. */
static void record_run(void) {
    struct timeval me;
    if (proc_start(getpid(), &me)) return;
    char line[96];
    int n = snprintf(line, sizeof(line), "%ld %ld %ld %lx\n", (long)getpid(), (long)me.tv_sec, (long)me.tv_usec,
                     bm.attached ? (unsigned long)(bm.cave - bm.base) : 0UL);
    int fd = open(MANAGER_PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (n > 0 && write(fd, line, (size_t)n) != n) log_line("process record: write failed");
    close(fd);
}

/* The payload loader names every payload process "payload.elf". Show the release file name instead: find the current
   name (ki_comm) in the first 0x1000 bytes of this process's struct proc and write the new p_comm there. */
static void set_process_name(const char *name) {
    static unsigned char kb[0x1000], pb[0x1000];
    size_t n = sizeof(kb); int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc *kp = (void *)kb;          /* the console's kinfo_proc is larger than the SDK's */
    if (sysctl(mib, 4, kb, &n, NULL, 0) || !kp->ki_comm[0] || !strcmp(kp->ki_comm, name)) return;
    char cur[20] = {0}; strncpy(cur, kp->ki_comm, sizeof(cur) - 1);
    intptr_t p = kernel_get_proc(getpid());
    if (!p || kernel_copyout(p, pb, sizeof(pb))) { log_line("process name: struct proc not readable"); return; }
    size_t l = strlen(cur) + 1;
    for (size_t o = 0; o + 20 <= sizeof(pb); o++)
        if (!memcmp(pb + o, cur, l)) {
            char z[20] = {0}; strncpy(z, name, sizeof(z) - 1);
            int rc = kernel_copyin(z, p + (intptr_t)o, sizeof(z));
            log_line("process name: %s -> %s (struct proc +0x%zx, rc %d)", cur, name, o, rc);
            return;
        }
    log_line("process name: %s not found in struct proc", cur);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    int srv = panel_socket();
    char msg[256];
    if (srv < 0) {
        snprintf(msg, sizeof(msg), "Web panel port %d unavailable; nothing changed", PORT);
        notify(msg); puts(msg);
        return 1;
    }
    log_line("%s started", VERSION);
    set_process_name("neo-manager.elf");
    sceUserServiceInitialize(0);
    settings_load();
    log_line("settings: force_neo=%d force_8gib=%d gpu_clock_enabled=%d gpu_clock_mhz=%u",
             force_neo, force_8gib, gpu.enabled, gpu.target);
    do_set(1, msg, sizeof(msg), 0);
    tsc_hz = tsc_rate();
    launch_attach(1);
    log_line("launch: questions %s (TSC %llu Hz, wait %d s)", live_ok ? "enabled" : "unavailable, hook not installed",
             (unsigned long long)tsc_hz, LAUNCH_WAIT_S);
    bm_reuse_allowed = previous_run_gone();
    memory_init(fw_raw_version(), tsc_hz * 3);
    record_run();
    memory_wants8 = memory_launch_wants;
    memory_refused8 = memory_launch_refused;
    if (bm.attached && !memory_data_blocked && bm_field(MS_ENABLED, 1)) {
        memory_data_blocked = 1;
        log_line("No-HV mount hook enable failed; reboot required");
    }
    drop_8gib_request();
    if (force_8gib) memory_request(1);
    int memory_previous_result=memory_poll();
    log_line("8GB Mode: %s", memory_message);
    gpu_tick();
    int li = install_launcher();
    snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "\nWeb panel: port %d%s", PORT,
             li == 1 ? "\nShortcut installed in the Media tab" : li < 0 ? "\nFailed to install the Media tab shortcut" : "");
    notify(msg); puts(msg);
    log_state();
    patches_update();
    patches_drop_8gib(NULL);
    time_t last_memory_poll=time(NULL),last_loop=last_memory_poll,last_reopen=0,last_probe=last_memory_poll;
    long long last_mirror_ms=0;
    for (;;) {
        memory_data_tick();
        launch_poll();
        play_tick();
        binary_tick();
        mp_tick();
        time_t now=time(NULL);
        /* Rest mode freezes this process and leaves the panel socket dead while the process lives on. The loop runs
           every 20 ms, so a long gap means it was frozen: open the panel socket again (a false gap only reopens it). */
        if (srv >= 0 && (now - last_loop > 30 || now < last_loop)) {
            log_line("panel: loop paused %ld s (rest mode); reopening the web panel socket", (long)(now - last_loop));
            close(srv); srv = -1;
        }
        last_loop = now;
        if (srv >= 0 && (now - last_probe >= 10 || now < last_probe)) {
            last_probe = now;
            if (!panel_alive()) {
                log_line("panel: port %d refuses connections; reopening the web panel socket", PORT);
                close(srv); srv = -1;
            }
        }
        if (srv < 0) {
            if (now != last_reopen) {
                last_reopen = now;
                if ((srv = panel_socket()) >= 0) log_line("panel: web panel listening again on port %d", PORT);
            }
        }
        { struct timespec mt; clock_gettime(CLOCK_MONOTONIC, &mt);
          long long ms = (long long)mt.tv_sec * 1000 + mt.tv_nsec / 1000000;
          if (ms - last_mirror_ms >= 250 || ms < last_mirror_ms) { log_mirror_tick(); last_mirror_ms = ms; } }
        if(now<last_memory_poll||now-last_memory_poll>=2){
            launch_attach(-1);
            hook_retry();
            launch_tick();
            settings_watch();
            gpu_tick();
            int memory_result=memory_poll();
            if(memory_result!=memory_previous_result){
                if(memory_result==0||memory_result==5||memory_result==6)notify(memory_message);
                else log_line("8GB Mode: %s (result %d)",memory_message,memory_result);
                drop_8gib_request();
                log_state();
                patches_drop_8gib(NULL);
            }
            memory_previous_result=memory_result;last_memory_poll=now;
        }
        /* short wait: launch questions are answered between requests */
        if(srv<0){usleep(20000);continue;}             /* launch questions and polls above keep running */
        fd_set ready;FD_ZERO(&ready);FD_SET(srv,&ready);struct timeval wait={0,pending.active?2000:20000};
        int selected=select(srv+1,&ready,NULL,NULL,&wait);
        if(selected<0&&errno!=EINTR){log_line("panel: select failed (errno %d); reopening the web panel socket",errno);close(srv);srv=-1;continue;}
        if(selected<=0)continue;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[4096], *form_body = NULL;
        if (neo_http_read(fd, req, sizeof req, &form_body)) {
            reply(fd, "400 Bad Request", "text/plain", "Invalid or incomplete request\n"); close(fd); continue;
        }
        char method[8] = "", path[64] = "";
        sscanf(req, "%7s %63s", method, path);
        int post = !strcmp(method, "POST");
        legacy_request = !strncmp(path, "/legacy", 7) || (form_body && strstr(form_body, "legacy=1"));
        if (!strcmp(path, "/legacy") || !strcmp(path, "/legacy/") || !strncmp(path, "/legacy?", 8)) {
            char q[128] = "";
            if (post) form_text(form_body, "q", q, sizeof q);
            serve_legacy(fd, NULL, q);
            close(fd); continue;
        }
        if (!strncmp(path, "/legacy/patches", 15)) {
            char tid[16] = "", hash[16] = "", on[4] = "", q[128] = "", pmsg[2048] = "";
            if (post) { form_text(form_body, "tid", tid, sizeof tid); form_text(form_body, "q", q, sizeof q); }
            else if (!strncmp(path, "/legacy/patches?tid=", 20)) snprintf(tid, sizeof tid, "%.9s", path + 20);
            if (post && !strcmp(path, "/legacy/patches/set")) {
                form_text(form_body, "hash", hash, sizeof hash); form_text(form_body, "on", on, sizeof on);
                if (strcmp(on, "0") && strcmp(on, "1")) snprintf(pmsg, sizeof pmsg, "Invalid patch request");
                else game_set_patch(tid, hash, on[0] == '1', memory_available(), pmsg, sizeof pmsg);
            }
            serve_legacy_patches(fd, tid, q, pmsg);
            close(fd); continue;
        }
        if (post && !strcmp(path, "/legacy/play")) {
            char tid[16] = "", pmsg[400];
            form_text(form_body, "tid", tid, sizeof tid);
            play_start(tid, pmsg, sizeof pmsg);
            serve_legacy(fd, pmsg, "");
            close(fd); continue;
        }
        if (post && !strcmp(path, "/patches/set")) {
            char tid[16], hash[16], on[4], pmsg[2048];
            if (form_value(form_body, "tid", tid, sizeof tid) || form_value(form_body, "hash", hash, sizeof hash) ||
                form_value(form_body, "on", on, sizeof on) || (strcmp(on, "0") && strcmp(on, "1"))) {
                reply(fd, "400 Bad Request", "text/plain", "Invalid patch request\n");
            } else if (game_set_patch(tid, hash, on[0] == '1', memory_available(), pmsg, sizeof pmsg)) {
                reply(fd, "409 Conflict", "text/plain; charset=utf-8", pmsg);
            } else {
                /* message, then "state" and every entry of the game as hash=0/1: the panel shows what changed */
                char name[128], ver[16]; int pn = 0; size_t sl = 0;
                gp_patch *pp = game_info(tid, name, sizeof name, ver, sizeof ver) ? NULL : game_patches(tid, ver, &pn);
                char *st = settings_read(&sl);
                size_t at = strlen(pmsg);
                at += (size_t)snprintf(pmsg + at, sizeof pmsg - at, "\nstate");
                for (int i = 0; i < pn && at < sizeof pmsg; i++)
                    at += (size_t)snprintf(pmsg + at, sizeof pmsg - at, " %s=%d", pp[i].hash, gp_setting(st, sl, pp[i].hash) == 1);
                free(pp); free(st);
                reply(fd, "200 OK", "text/plain; charset=utf-8", pmsg);
            }
        } else if (post && !strcmp(path, "/games/play")) {
            char tid[16], pmsg[400];
            if (form_value(form_body, "tid", tid, sizeof tid)) reply(fd, "400 Bad Request", "text/plain", "Invalid game\n");
            else if (play_start(tid, pmsg, sizeof pmsg)) reply(fd, "409 Conflict", "text/plain; charset=utf-8", pmsg);
            else reply(fd, "200 OK", "text/plain; charset=utf-8", pmsg);
        } else if (post && (!strcmp(path, "/advanced/neo") || !strcmp(path, "/advanced/memory"))) {
            char on[4]; int value = !form_value(form_body, "on", on, sizeof on) && !strcmp(on, "1");
            const char *amsg;
            if (!strcmp(path, "/advanced/neo")) {
                force_neo = value; launch_attach(1);
                amsg = value ? "Pro mode for every PS4 game: on" : "Pro mode for every PS4 game: off (follows the game patches)";
            } else if (value && !memory_available()) {
                amsg = "8GB Mode needs firmware 13.20 or 13.60 with a validated memory launch hook enabled; nothing changed";
            } else {
                force_8gib = value;
                if (value) memory_request(1); else if (!auto8.active) memory_request(0);
                amsg = value ? "Keep 8GB Mode on: on" : "Keep 8GB Mode on: off (8GB Mode follows the game patches)";
            }
            int saved = !settings_save();
            log_line("panel: %s%s", amsg, saved ? "" : " (settings not saved)");
            notify(amsg); log_state(); serve_page(fd, amsg);
        } else if (post && (!strcmp(path, "/gpu/enable") || !strcmp(path, "/gpu/disable"))) {
            int enable = !strcmp(path, "/gpu/enable"); unsigned target = gpu.target;
            if (enable && !gpu_available()) {     /* disable stays allowed: it restores the captured clock */
                reply(fd, "404 Not Found", "text/plain", "GPU clock control is not available on this console\n");
            } else if (enable && gpu_form_mhz(form_body, &target) != 1) {
                reply(fd, "400 Bad Request", "text/plain", "Choose a GPU target from 1800 to 2200 MHz in 50 MHz steps\n");
            } else {
                int rc = gpu_request(enable, target, gpu_game_pid());
                if (rc == -1 && enable) snprintf(gpu.message, sizeof gpu.message, "GPU clock request unavailable or failed; check firmware and SMU access");
                log_line("panel: GPU enable=%d target=%u rc=%d", enable, target, rc);
                notify(gpu.message); log_state(); serve_gpu_page(fd, gpu.message);
            }
        } else if (post && !strcmp(path, "/patches/update")) {
            log_line("panel: game patches update requested");
            patches_update();
            serve_page(fd, patches_message);
        } else if (post && !strcmp(path, "/settings/save")) {
            int ok = !settings_save();
            const char *smsg = ok ? "Settings saved" : "Failed to save settings";
            log_line("panel: save settings force_neo=%d force_8gib=%d gpu_clock_enabled=%d gpu_clock_mhz=%u",
                     force_neo, force_8gib, gpu.enabled, gpu.target);
            notify(smsg);
            if (!strcmp(form_body, "view=gpu") && gpu_available()) serve_gpu_page(fd, smsg); else serve_page(fd, smsg);
        } else if (post && !strcmp(path, "/quit")) {
            if (memory_shutdown()) { reply(fd, "409 Conflict", "text/plain", "Close the game and wait for DATA restoration before quitting; check memory status\n"); close(fd); continue; }
            if (gpu_request(0, gpu.target, gpu_game_pid())) {
                notify(gpu.message); serve_gpu_page(fd, "Clock restoration failed. Retry Disable before quitting."); close(fd); continue;
            }
            reply(fd, "200 OK", "text/html; charset=utf-8", "<!doctype html><meta charset=utf-8><title>NeoEnabler</title>"
                  "<body style='background:#111418;color:#e8eaed;font-family:system-ui'>Manager stopped. Games no longer ask for Pro mode "
                  "or 8GB Mode until the manager runs again. GPU clock restore requested.</body>");
            close(fd);
            launch_attach(0);
            notify("NeoEnabler: panel stopped (per-game Pro mode and 8GB Mode stopped, GPU boost stopped)");
            log_state();
            break;
        } else if (!strcmp(path, "/status")) {
            shell_t s; int st = hook_state(&s);
            char games[1024]; games_text(games, sizeof(games), 0);
            char body[3200];mb_snapshot memory;memory_check(&memory);gpu_refresh();
            snprintf(body, sizeof(body), "build: " VERSION "\nmemory_8gib: %s\nmemory_requested: %s\nmemory_detail: %s\nmemory_last_result: %s\nhook: %s\nv4 hook: %s\nfw: %x site: %lx target: %lx cave: %lx eh_frame: %lx-%lx attr: 0x%x four_args: %d\n%s",
                     memory.state==MB_8GIB?"enabled":memory.state==MB_UNKNOWN?"unavailable":"disabled",
                     memory_target?"enabled":"disabled",memory.detail,memory_message,
                     state_text(st), v4_active(&s) ? "present" : "absent", fw_major(), s.site, s.target, s.cave, s.frame, s.frame_end,
                     s.attr_off, s.four_args, games);
            size_t used = strlen(body);
            snprintf(body + used, sizeof body - used,
                     "gpu_clock: %s\ngpu_clock_enabled: %d\ngpu_target_mhz: %u\ngpu_actual_mhz: %s%u\ngpu_detail: %s\n",
                     gpu_state_text(), gpu.enabled, gpu.target, gpu.reading_valid ? "" : "unavailable/last=", gpu.actual, gpu.message);
            used = strlen(body);
            snprintf(body + used, sizeof body - used,
                     "force_neo: %d\nforce_8gib: %d\nlaunch_questions: %s\nlast_question: %u\n8gib_for_launch: %s\n"
                     "attr_detected: %s 0x%x\n",
                     force_neo, force_8gib, live_ok ? "enabled" : "unavailable", last_seq, auto8.active ? auto8.tid : "none",
                     last_attr.tid[0] ? last_attr.tid : "none", last_attr.offset);
            used = strlen(body);
            bm_mailbox mail = {0}; int mail_ok = !bm_mail(&mail);
            snprintf(body + used, sizeof body - used,
                     "memory_backend: %s\nmount_hook: %s\nmount_preflight: %s\nmount_check_address: 0x%lx\n"
                     "mount_base: 0x%lx\nmount_text_field_raw: 0x%lx\nmount_map_size: 0x%lx\nmount_frame_header: 0x%lx\n"
                     "mount_padding_bytes: %s\nmount_slot_nonzero_bytes: %s\nmount_seq: %u\nmount_phase: %u\nmount_gate: %u\nmount_dirty: %u\n"
                     "data_owned: %d\ndata_restored: %d\ndata_constructor: %d\ndata_blocked: %d\n",
                     memory_data_mode ? "DATA (no HV)" : "TEXT (HV required)", bm.attached ? "installed" : "absent",
                     bm.failure[0] ? bm.failure : "no failure recorded", (unsigned long)bm.check_address,
                     (unsigned long)bm.base,(unsigned long)bm.main_text_size,(unsigned long)bm.main_map_size,(unsigned long)bm.frame_header,
                     bm.failure_preview[0] ? bm.failure_preview : "none",bm.slot_summary[0] ? bm.slot_summary : "not sampled",
                     mail_ok ? mail.seq : 0, mail_ok ? mail.phase : 0, mail_ok ? mail.gate : 0, mail_ok ? mail.dirty : 0,
                     memory_transaction.owned, memory_transaction.restored, memory_constructor, memory_data_blocked);
            reply(fd, "200 OK", "text/plain; charset=utf-8", body);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/panel.js")) {
            char script[sizeof panel_js + 1];
            memcpy(script, panel_js, sizeof panel_js); script[sizeof panel_js] = 0;
            reply(fd, "200 OK", "application/javascript; charset=utf-8", script);
        } else if (!strcmp(method, "GET") && !strncmp(path, "/icon/", 6)) {
            serve_icon(fd, path + 6);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/gpu")) {
            if (gpu_available()) serve_gpu_page(fd, NULL);
            else serve_page(fd, "GPU clock control is not available on this console");
        } else if (!strcmp(method, "GET") && !strcmp(path, "/gpu/status")) {
            if (gpu_available()) serve_gpu_status(fd);
            else reply(fd, "404 Not Found", "text/plain", "not found\n");
        } else if (!strcmp(method, "GET") && !strcmp(path, "/logs")) {
            serve_logs_show(fd);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/logs/download")) {
            serve_logs_download(fd);
        } else if (post && !strcmp(path, "/logs/save")) {
            serve_logs_save(fd);
        } else if (post && !strcmp(path, "/diagnostic/save")) {
            serve_diagnostic_save(fd);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/diagnostic")) {
            serve_diagnostic(fd);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/")) {
            serve_page(fd, NULL);
        } else {
            reply(fd, "404 Not Found", "text/plain", "not found\n");
        }
        close(fd);
    }
    close(srv);
    return 0;
}
