/* btdeep: the deep dive into Sony's BT stack.
 *
 *   1. Dumps ALL code from libSceHidControl.sprx (the possible connection API)
 *   2. Dumps ALL code from libSceBluetoothHid.sprx (remaining functions)
 *   3. Enumerates every export from both libraries
 *   4. Scans kernel memory for VID/PID tables (the bthid allowlist)
 *   5. Probes /dev/bt with structured parameters
 *
 * Everything saved to /data/btfix-out/ for organized collection.
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
#include <sys/mman.h>
#include <sys/stat.h>

static FILE *g_out;
static const char *k_dir = "/data/btfix-out";

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_out ? g_out : stdout, "%s\n", buf);
    fprintf(stdout, "%s\n", buf);
    if (g_out) fflush(g_out);
}

/* ---- memory dump via pipe (bypasses execute-only pages) ---------------- */

static long dump_via_pipe(const char *name, uintptr_t addr, int bytes)
{
    char path[512];
    int pipefd[2];
    unsigned char *p = (unsigned char *)addr;
    FILE *f;
    long written = 0;

    snprintf(path, sizeof path, "%s/%s.bin", k_dir, name);
    if (pipe(pipefd) != 0) return -1;

    /* try to write from the target address to the pipe */
    int n = write(pipefd[1], p, bytes);
    if (n <= 0) {
        close(pipefd[0]); close(pipefd[1]);
        log_ts("  pipe read failed for %s (errno %d)", name, errno);
        return -1;
    }

    /* read back and save */
    unsigned char *buf = malloc(n);
    int got = read(pipefd[0], buf, n);
    close(pipefd[0]); close(pipefd[1]);

    if (got <= 0) { free(buf); return -1; }
    f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, got, f);
        fclose(f);
        written = got;
    }
    free(buf);
    log_ts("  dumped %s: %ld bytes from %p", name, written, (void *)addr);
    return written;
}

/* Also try direct read with mprotect fallback */
static long dump_mem(const char *name, uintptr_t addr, int bytes)
{
    long r = dump_via_pipe(name, addr, bytes);
    if (r > 0) return r;

    /* fallback: mprotect + direct */
    {
        char path[512];
        FILE *f;
        uintptr_t page = addr & ~0xFFFULL;
        size_t len = ((addr + bytes + 0xFFF) & ~0xFFFULL) - page;

        if (mprotect((void *)page, len, PROT_READ | PROT_EXEC) == 0) {
            snprintf(path, sizeof path, "%s/%s.bin", k_dir, name);
            f = fopen(path, "wb");
            if (f) {
                fwrite((void *)addr, 1, bytes, f);
                fclose(f);
                log_ts("  dumped %s via mprotect: %d bytes", name, bytes);
                return bytes;
            }
        }
    }
    log_ts("  FAILED to dump %s", name);
    return -1;
}

/* ---- export enumeration -------------------------------------------------- */

/* Try a comprehensive list of names for both libraries */
static const char *const k_hc_names[] = {
    "sceHidControlInit", "sceHidControlTerm",
    "sceHidControlConnect", "sceHidControlDisconnect",
    "sceHidControlRegisterDevice", "sceHidControlUnregisterDevice",
    "sceHidControlGetDeviceInfo", "sceHidControlSetDeviceInfo",
    "sceHidControlOpen", "sceHidControlClose",
    "sceHidControlStart", "sceHidControlStop",
    "sceHidControlScan", "sceHidControlPair",
    "sceHidControlAccept", "sceHidControlReject",
    "sceHidControlSend", "sceHidControlReceive",
    "sceHidControlSetCallback", "sceHidControlGetCallback",
    "sceHidControlGetVersion", "sceHidControlGetStatus",
    "sceHidControlIoctl", "sceHidControlCommand",
    NULL,
};

static const char *const k_bh_names[] = {
    "sceBluetoothHidInit", "sceBluetoothHidTerm",
    "sceBluetoothHidRegisterDevice", "sceBluetoothHidUnregisterDevice",
    "sceBluetoothHidGetDeviceInfo", "sceBluetoothHidSetDeviceInfo",
    "sceBluetoothHidConnect", "sceBluetoothHidDisconnect",
    "sceBluetoothHidPair", "sceBluetoothHidUnpair",
    "sceBluetoothHidScan", "sceBluetoothHidStopScan",
    "sceBluetoothHidOpen", "sceBluetoothHidClose",
    "sceBluetoothHidGetConnectionList", "sceBluetoothHidGetDeviceList",
    "sceBluetoothHidSetCallback", "sceBluetoothHidGetCallback",
    "sceBluetoothHidSend", "sceBluetoothHidReceive",
    "sceBluetoothHidSetReport", "sceBluetoothHidGetReport",
    "sceBluetoothHidStartInquiry", "sceBluetoothHidStopInquiry",
    "sceBluetoothHidAcceptConnection", "sceBluetoothHidRejectConnection",
    "sceBluetoothHidGetVersion", "sceBluetoothHidGetStatus",
    NULL,
};

static void enumerate_lib(void *lib, const char *libname,
                          const char *const *names)
{
    log_ts("-- exports of %s:", libname);
    int found = 0;
    for (const char *const *n = names; *n; n++) {
        void *p = dlsym(lib, *n);
        if (p) {
            log_ts("  EXPORT: %s at %p", *n, p);
            found++;
        }
    }
    if (!found) log_ts("  (none of the known names)");
}

/* ---- kernel VID/PID scan ------------------------------------------------ */

/* Search our process's accessible memory for Sony VID/PID patterns.
 * The kernel's allowlist would be in kernel space, but system libraries
 * loaded into our process might have copies of the tables. */
static void scan_user_memory(void)
{
    log_ts("== scanning accessible memory for VID/PID tables:");

    /* Known Sony VID = 0x054c. In a table, it would appear as consecutive
     * 16-bit LE values like: 4c 05 xx xx 4c 05 yy yy 4c 05 zz zz... */

    /* We can't easily enumerate all mappings, but we can check the
     * loaded libraries' data sections by reading around their globals */

    /* For now, we scan the first 1 MB after each known function address,
     * looking for VID/PID patterns */
    {
        void *lib = dlopen("/system/common/lib/libSceBluetoothHid.sprx", RTLD_NOW);
        if (lib) {
            void *init = dlsym(lib, "sceBluetoothHidInit");
            if (init) {
                /* scan around the library's data area */
                /* The globals we saw in the disassembly were at offsets
                 * 0x7700-0x7800 and 0xb700-0xb800 from the function base */
                uintptr_t base = (uintptr_t)init;
                log_ts("  scanning around libSceBluetoothHid (base %p):", (void *)base);

                /* look for the initialization flag and fd we found in the disasm */
                /* The disasm showed: cmpb $0x1, 0xb71f(%rip) → init flag
                 *                    cmpq $0x0, 0xb713(%rip) → fd
                 * These are at approximately base+0x7780 and base+0x7788 */
                dump_mem("bh_globals", base + 0x7700, 256);
                dump_mem("bh_late_globals", base + 0xb700, 256);
            }
        }
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    char path[512];
    void *bh, *hc;

    mkdir(k_dir, 0777);
    mkdir("/data/btfix-out/dumps", 0777);
    snprintf(path, sizeof path, "%s/btdeep.log", k_dir);
    g_out = fopen(path, "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btdeep: the deep dive into Sony's BT stack");

    /* load both libraries */
    bh = dlopen("/system/common/lib/libSceBluetoothHid.sprx", RTLD_NOW | RTLD_GLOBAL);
    hc = dlopen("/system/common/lib/libSceHidControl.sprx", RTLD_NOW | RTLD_GLOBAL);
    log_ts("libSceBluetoothHid: %s", bh ? "loaded" : dlerror());
    log_ts("libSceHidControl: %s", hc ? "loaded" : dlerror());

    /* enumerate exports of both */
    if (hc) {
        enumerate_lib(hc, "libSceHidControl", k_hc_names);
        /* dump ALL found functions */
        for (const char *const *n = k_hc_names; *n; n++) {
            void *p = dlsym(hc, *n);
            if (p) {
                char name[64];
                snprintf(name, sizeof name, "hc_%s", *n + 3); /* strip "sce" */
                dump_mem(name, (uintptr_t)p, 1024);
            }
        }
    }

    if (bh) {
        enumerate_lib(bh, "libSceBluetoothHid", k_bh_names);
        /* dump any we haven't dumped yet */
        for (const char *const *n = k_bh_names; *n; n++) {
            void *p = dlsym(bh, *n);
            if (p) {
                char name[64];
                snprintf(name, sizeof name, "bh_%s", *n + 3);
                dump_mem(name, (uintptr_t)p, 1024);
            }
        }
    }

    /* scan for VID/PID tables in accessible memory */
    scan_user_memory();

    /* also dump the SPRX files from disk if we haven't already */
    {
        const char *sprx[] = {
            "/system/common/lib/libSceHidControl.sprx",
            "/system/common/lib/libSceBluetoothHid.sprx",
            NULL,
        };
        for (const char *const *s = sprx; *s; s++) {
            FILE *src = fopen(*s, "rb");
            if (src) {
                char out[512];
                unsigned char buf[65536];
                size_t n;
                int total = 0;
                snprintf(out, sizeof out, "%s/dumps/%s", k_dir, strrchr(*s, '/') + 1);
                FILE *dst = fopen(out, "wb");
                if (dst) {
                    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
                        fwrite(buf, 1, n, dst);
                        total += n;
                    }
                    fclose(dst);
                    log_ts("dumped %s (%d bytes)", *s, total);
                }
                fclose(src);
            }
        }
    }

    /* /dev/bt: structured probe with various vid/pid registrations */
    log_ts("== /dev/bt structured probe:");
    {
        int fd = open("/dev/bt", O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            /* first: Init the library properly */
            if (bh) {
                int32_t (*init_fn)(void *) = dlsym(bh, "sceBluetoothHidInit");
                if (init_fn) {
                    int r = init_fn(NULL);
                    log_ts("  Init: %#x", (unsigned)r);
                }
            }

            /* try RegisterDevice with Sony VID and various PIDs */
            if (bh) {
                int32_t (*reg_fn)(uint32_t, uint32_t) = dlsym(bh, "sceBluetoothHidRegisterDevice");
                if (reg_fn) {
                    struct { uint16_t vid, pid; const char *what; } tests[] = {
                        { 0x054c, 0x0ce6, "DualSense" },
                        { 0x054c, 0x05c4, "DS4 v1" },
                        { 0x054c, 0x09cc, "DS4 v2" },
                        { 0x2dc8, 0x3012, "8BitDo Ultimate" },
                        { 0x2dc8, 0x2002, "8BitDo SN30" },
                        { 0x0000, 0x0000, "null" },
                        { 0xffff, 0xffff, "wildcard" },
                    };
                    for (int i = 0; i < 7; i++) {
                        int r = reg_fn(tests[i].vid, tests[i].pid);
                        log_ts("  Register(%04x:%04x = %s): %#x %s",
                               tests[i].vid, tests[i].pid, tests[i].what,
                               (unsigned)r, r == 0 ? "OK!" : "");
                    }
                }
            }

            /* try reading after registration */
            {
                unsigned char ev[64];
                int n = read(fd, ev, sizeof ev);
                if (n > 0) {
                    char hex[3 * 32 + 1];
                    for (int i = 0; i < n && i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", ev[i]);
                    hex[3 * (n < 32 ? n : 32)] = 0;
                    log_ts("  read after register: %d bytes: %s", n, hex);
                }
            }

            close(fd);
        }
    }

    log_ts("btdeep: done");
    if (g_out) fclose(g_out);
    return 0;
}
