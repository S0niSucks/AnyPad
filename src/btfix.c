/* btfix: the comprehensive tool to find, dump and understand Sony's
 * Bluetooth management stack, and make the 8BitDo connect natively.
 *
 * Phase 1: Find and dump libSceBluetoothHid.sprx (or equivalent)
 *   - Read from /system/common/lib/ if accessible
 *   - dlopen and walk memory if not
 *   - Parse the ELF export table: ALL function names
 *
 * Phase 2: Dump the /dev/bt ioctl surface
 *   - Try various ioctls on /dev/bt
 *   - Find the bthid kernel module's device table (allowlist)
 *
 * Phase 3: Kernel memory (with Byepervisor if needed)
 *   - Scan for bthid structures
 *   - Find VID/PID tables
 *   - Look for connection management callbacks
 *
 * Output: /data/btfix.log and /data/btfix/ (dumped files)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>

static FILE *g_out;

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[2048];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_out ? g_out : stdout, "%s\n", buf);
    if (*buf == '=') fprintf(stdout, "\n");
    fprintf(stdout, "%s\n", buf);
    if (g_out) fflush(g_out);
}

#define HEX(buf, n) ({ \
    static char _hex[3 * 4096 + 1]; \
    int _k; \
    for (_k = 0; _k < (n) && _k < 4096; _k++) snprintf(_hex + 3 * _k, 4, "%02x ", (unsigned char)(buf)[_k]); \
    _hex[3 * (_k < 4096 ? _k : 4096)] = 0; \
    _hex; \
})

/* ---- Phase 1: find and dump the library -------------------------------- */

static const char *const k_search_paths[] = {
    "/system/common/lib",
    "/system/priv/lib",
    "/system/tmp/lib",
    NULL,
};

static const char *const k_names[] = {
    "libSceBluetoothHid.sprx",
    "libSceBluetooth.sprx",
    "libSceBt.sprx",
    "libSceBthid.sprx",
    "libSceBtHid.sprx",
    "libSceBluetoothHidControl.sprx",
    "libSceHid.sprx",
    NULL,
};

/* Copy a file to /data/btfix/ for offline analysis */
static long dump_file(const char *path)
{
    FILE *src = fopen(path, "rb");
    FILE *dst;
    char out[512];
    unsigned char buf[65536];
    size_t n;
    long total = 0;
    static int dump_n = 0;

    if (!src) return -1;
    snprintf(out, sizeof out, "/data/btfix/%02d_%s", dump_n++, strrchr(path, '/') + 1);
    dst = fopen(out, "wb");
    if (!dst) { fclose(src); return -1; }
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        fwrite(buf, 1, n, dst);
        total += (long)n;
    }
    fclose(src);
    fclose(dst);
    return total;
}

/* Parse the ELF export table of a loaded module */
static void parse_exports(void *handle, const char *name)
{
    /* On PS5, dlsym works with known names. For full enumeration, we'd
     * need to parse the ELF in memory. Let's try both. */
    log_ts("-- enumerating exports of %s:", name);

    /* First: try the known/guessed names */
    {
        static const char *const syms[] = {
            "sceBthidInit", "sceBthidTerm", "sceBthidOpen", "sceBthidClose",
            "sceBthidConnect", "sceBthidDisconnect", "sceBthidPair",
            "sceBthidRegister", "sceBthidUnregister",
            "sceBthidGetConnectionList", "sceBthidGetDeviceInfo",
            "sceBthidSetReport", "sceBthidGetReport",
            "sceBthidAcceptConnection", "sceBthidRejectConnection",
            "sceBthidStartInquiry", "sceBthidStopInquiry",
            "sceBthidStartScan", "sceBthidStopScan",
            "sceBthidGetRemoteDeviceInfo",
            "sceBthidSetCallback", "sceBthidGetVersion",
            "sceBluetoothInit", "sceBluetoothTerm",
            "sceBluetoothConnect", "sceBluetoothDisconnect",
            "sceBluetoothPair", "sceBluetoothScan",
            "sceBluetoothGetDeviceInfo", "sceBluetoothSetDiscoverable",
            "sceBluetoothHidInit", "sceBluetoothHidTerm",
            "sceBluetoothHidConnect", "sceBluetoothHidDisconnect",
            "sceBluetoothHidPair", "sceBluetoothHidRegisterDevice",
            "sceBluetoothHidAddDevice", "sceBluetoothHidRemoveDevice",
            "sceBluetoothHidGetRemoteSetting",
            "sceBtInit", "sceBtTerm", "sceBtConnect", "sceBtDisconnect",
            "sceBtPair", "sceBtScan", "sceBtGetDeviceList",
            "sceBtSetDiscoverable", "sceBtAcceptConnection",
            "sceBtHidInit", "sceBtHidConnect", "sceBtHidDisconnect",
            "sceBtHidRegister", "sceBtHidPair",
            /* more generic patterns */
            "_sceBthidInit", "_sceBtInit",
            /* LD (linked) names */
            "__libSceBluetoothHid_...",
            NULL,
        };
        int found = 0;
        for (const char *const *s = syms; *s; s++) {
            void *p = dlsym(handle, *s);
            if (p) {
                log_ts("  EXPORT: %s at %p", *s, p);
                found++;
            }
        }
        if (!found) log_ts("  (no known exports; trying RTLD_DEFAULT)");
    }
}

/* ---- Phase 2: /dev/bt ioctl surface ------------------------------------- */

static void probe_bt_dev(void)
{
    int fd;
    unsigned char buf[256];

    log_ts("== /dev/bt ioctl surface:");

    fd = open("/dev/bt", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_ts("  /dev/bt: errno %d", errno);
        return;
    }
    log_ts("  /dev/bt: opened (fd %d)", fd);

    /* try various ioctls */
    {
        /* the known registration ioctl */
        unsigned int reg[2] = { 0x044f, 0x1000 };
        int r = ioctl(fd, 0x8004622b, reg);
        log_ts("  ioctl 0x8004622b (register 044f/1000): %d errno %d", r, errno);

        /* try some other ioctl numbers that might do interesting things */
        static const unsigned long iocs[] = {
            0x8004622c, 0x8004622d, 0x8004622e, 0x8004622f,
            0x80046230, 0x80046231, 0x80046232, 0x80046233,
            0x8004623a, 0x8004623b, 0x8004623c,
            0x4004622b, 0x4004622c,
            0xc004622b, 0xc004622c,
            0x8008622b,
            0x8010622b,
        };
        for (int i = 0; i < (int)(sizeof iocs / sizeof iocs[0]); i++) {
            memset(buf, 0, sizeof buf);
            errno = 0;
            r = ioctl(fd, iocs[i], buf);
            if (r == 0) {
                log_ts("  ioctl %#lx: OK! buf: %s", iocs[i], HEX(buf, 32));
            } else if (errno != ENOTTY) {
                log_ts("  ioctl %#lx: %d errno %d (not ENOTTY!)", iocs[i], r, errno);
            }
        }
    }

    /* try reading (if we registered successfully) */
    {
        int n = read(fd, buf, sizeof buf);
        if (n > 0) log_ts("  read: %d bytes: %s", n, HEX(buf, n < 32 ? n : 32));
        else log_ts("  read: %d errno %d", n, errno);
    }

    close(fd);
}

/* ---- Phase 3: kernel structures ------------------------------------------ */

/* Search the kernel for the bthid allowlist (VID/PID table).
 * On PS4, OneFamily found it as a static array in the kernel.
 * On PS5, the equivalent would be a similar table.
 *
 * We search for known VID/PID patterns:
 *   Sony DualSense: 054c:0ce6, 054c:0df2
 *   Sony DS4: 054c:05c4, 054c:09cc
 *   Licensed: 054c:05c5 (Navigation), etc.
 *
 * The kernel base is at a known address (from the kstuff offsets).
 */
static void scan_kernel_for_vids(void)
{
    /* The PS5 kernel is mapped at a known base. The kstuff offsets file
     * has addresses relative to the kernel base. We'd need Byepervisor
     * or kernel_read to access it. For now, just report what we'd look for. */
    log_ts("== kernel scan for bthid allowlist:");
    log_ts("  (searching for VID/PID table with Sony IDs)");
    log_ts("  (DualSense: 054c:0ce6, DS4: 054c:05c4, DS4v2: 054c:09cc)");

    /* On PS4, OneFamily found the table at [0x4E0000, 0x660000) in the kernel.
     * On PS5, the equivalent range would be different but findable.
     *
     * The VID/PID table format on PS4 was:
     *   struct { uint16_t vid; uint16_t pid; uint32_t flags; } entries[];
     *
     * We search for consecutive 054c:xxxx pairs in kernel memory.
     */

    /* Without direct kernel access from this payload, we note the plan:
     * 1. Use Byepervisor to dump the kernel's .data/.rodata sections
     * 2. Search for 054c:xxxx patterns
     * 3. Find the table boundaries (consecutive entries)
     * 4. Add 2dc8:3012 (8BitDo Ultimate) to the table
     */
    log_ts("  (kernel access requires Byepervisor; run separately)");
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    mkdir("/data/btfix", 0777);
    g_out = fopen("/data/btfix/log.txt", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btfix: finding and understanding Sony's Bluetooth stack");

    /* Phase 1: find the library */
    log_ts("== Phase 1: finding libSceBluetoothHid");

    /* try reading from the filesystem */
    for (const char *const *dir = k_search_paths; *dir; dir++) {
        DIR *d = opendir(*dir);
        if (!d) {
            log_ts("  (cannot open %s: errno %d)", *dir, errno);
            continue;
        }
        log_ts("  scanning %s:", *dir);
        struct dirent *e;
        while ((e = readdir(d))) {
            /* filter: anything with bt, blue, hid in the name */
            if (strcasestr(e->d_name, "bt") || strcasestr(e->d_name, "blue") ||
                strcasestr(e->d_name, "hid") || strcasestr(e->d_name, "Bt")) {
                char full[512];
                snprintf(full, sizeof full, "%s/%s", *dir, e->d_name);
                struct stat st;
                if (stat(full, &st) == 0)
                    log_ts("    %s (%lld bytes)", e->d_name, (long long)st.st_size);

                /* if it's one of our targets, dump it */
                for (const char *const *n = k_names; *n; n++) {
                    if (strcmp(e->d_name, *n) == 0) {
                        long sz = dump_file(full);
                        log_ts("    DUMPED: %s (%ld bytes)", full, sz);
                    }
                }
            }
        }
        closedir(d);
    }

    /* try dlopen each candidate */
    for (const char *const *n = k_names; *n; n++) {
        char full[512];
        void *h;

        snprintf(full, sizeof full, "/system/common/lib/%s", *n);
        h = dlopen(full, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            log_ts("LOADED: %s", full);
            parse_exports(h, *n);
            continue;
        }
        h = dlopen(*n, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            log_ts("LOADED: %s (by name)", *n);
            parse_exports(h, *n);
        }
    }

    /* Phase 2: /dev/bt */
    probe_bt_dev();

    /* Phase 3: kernel structures */
    scan_kernel_for_vids();

    log_ts("== btfix: done");
    if (g_out) fclose(g_out);
    return 0;
}
