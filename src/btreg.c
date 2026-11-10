/* btreg: calls Sony's sceBluetoothHidRegisterDevice to understand its API.
 *
 * We found two exports in libSceBluetoothHid.sprx:
 *   sceBluetoothHidInit at 8004841c0
 *   sceBluetoothHidRegisterDevice at 8004848d0
 *
 * This tool:
 *   1. Loads the library, calls Init
 *   2. Tries RegisterDevice with various parameter patterns
 *   3. Watches the kernel log for reactions
 *   4. Also tries more export names we haven't tested
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static FILE *g_out;

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

/* ---- klog watcher ------------------------------------------------ */

static int g_klog = -1;
static char g_kline[1024];
static size_t g_kline_len;

static int klog_open(void)
{
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons(3232);
        sin.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(fd, (struct sockaddr *)&sin, sizeof sin) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            return fd;
        }
        close(fd);
    }
    return open("/dev/klog", O_RDONLY | O_NONBLOCK);
}

static void klog_poll(void)
{
    char buf[2048];
    ssize_t n;
    for (int rounds = 0; rounds < 4 && g_klog >= 0; rounds++) {
        n = read(g_klog, buf, sizeof buf);
        if (n <= 0) return;
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n' && g_kline_len < sizeof g_kline - 1) {
                g_kline[g_kline_len++] = buf[i];
                continue;
            }
            g_kline[g_kline_len] = '\0';
            g_kline_len = 0;
            if (strstr(g_kline, "bt") || strstr(g_kline, "Bt") || strstr(g_kline, "BT") ||
                strstr(g_kline, "luetooth") || strstr(g_kline, "mbus") ||
                strstr(g_kline, "LOGIN MGR") || strstr(g_kline, "[Umm]") ||
                strstr(g_kline, "pad") || strstr(g_kline, "Pad") ||
                strstr(g_kline, "DUALSENSE") || strstr(g_kline, "ualSense") ||
                strstr(g_kline, "error") || strstr(g_kline, "ERROR"))
                log_ts("sys: %s", g_kline);
        }
    }
}

/* ---- more export names to try -------------------------------------- */

static const char *const k_more_names[] = {
    "sceBluetoothHidRegisterDevice",
    "sceBluetoothHidUnregisterDevice",
    "sceBluetoothHidConnect",
    "sceBluetoothHidDisconnect",
    "sceBluetoothHidGetConnectionStatus",
    "sceBluetoothHidGetDeviceInfo",
    "sceBluetoothHidSetCallback",
    "sceBluetoothHidGetRemoteSetting",
    "sceBluetoothHidStartScan",
    "sceBluetoothHidStopScan",
    "sceBluetoothHidPair",
    "sceBluetoothHidUnpair",
    "sceBluetoothHidAcceptConnection",
    "sceBluetoothHidRejectConnection",
    "sceBluetoothHidGetDeviceList",
    "sceBluetoothHidSetReport",
    "sceBluetoothHidGetReport",
    "sceBluetoothHidOpen",
    "sceBluetoothHidClose",
    "sceBluetoothHidTerm",
    /* shorter names */
    "sceBtInit", "sceBtTerm", "sceBtConnect", "sceBtDisconnect",
    "sceBtPair", "sceBtScan", "sceBtRegister", "sceBtUnregister",
    "sceBtGetDeviceList", "sceBtSetCallback",
    "sceBtOpen", "sceBtClose", "sceBtGetStatus",
    /* HidControl */
    "sceHidControlInit", "sceHidControlTerm",
    "sceHidControlConnect", "sceHidControlDisconnect",
    "sceHidControlRegisterDevice",
    NULL,
};

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    void *lib;
    int32_t (*init_fn)(void *);
    int32_t (*reg_fn)(void *, ...);
    int r;

    g_out = fopen("/data/btreg.log", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btreg: calling sceBluetoothHidRegisterDevice");

    /* open the klog first (to watch the system react) */
    g_klog = klog_open();
    if (g_klog >= 0) log_ts("klog: open");

    /* load the library */
    lib = dlopen("/system/common/lib/libSceBluetoothHid.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        log_ts("FATAL: cannot load libSceBluetoothHid.sprx: %s", dlerror());
        if (g_out) fclose(g_out);
        return 1;
    }
    log_ts("libSceBluetoothHid.sprx loaded");

    /* try more export names */
    log_ts("-- additional exports:");
    for (const char *const *n = k_more_names; *n; n++) {
        void *p = dlsym(lib, *n);
        if (p) log_ts("  FOUND: %s at %p", *n, p);
    }
    /* also in HidControl */
    {
        void *hc = dlopen("/system/common/lib/libSceHidControl.sprx", RTLD_NOW | RTLD_GLOBAL);
        if (hc) {
            log_ts("libSceHidControl.sprx loaded");
            for (const char *const *n = k_more_names; *n; n++) {
                void *p = dlsym(hc, *n);
                if (p) log_ts("  FOUND (HidControl): %s at %p", *n, p);
            }
        }
    }

    /* find our two known functions */
    init_fn = (int32_t (*)(void *))dlsym(lib, "sceBluetoothHidInit");
    reg_fn = (int32_t (*)(void *, ...))dlsym(lib, "sceBluetoothHidRegisterDevice");

    if (!init_fn || !reg_fn) {
        log_ts("FATAL: functions not found");
        if (g_out) fclose(g_out);
        return 1;
    }

    /* call Init */
    log_ts("-- calling sceBluetoothHidInit:");
    r = init_fn(NULL);
    log_ts("  Init(NULL): %#x", (unsigned)r);
    klog_poll();

    /* try Init with a parameter */
    {
        uint32_t param[8] = { 0 };
        r = init_fn(param);
        log_ts("  Init(zeros): %#x", (unsigned)r);
    }

    /* call RegisterDevice with various patterns */
    log_ts("-- probing sceBluetoothHidRegisterDevice:");

    /* pattern 1: NULL (see if it returns the number of devices or an error) */
    r = reg_fn(NULL);
    log_ts("  Register(NULL): %#x", (unsigned)r);

    /* pattern 2: a BT address + VID/PID (what a controller registration might look like) */
    {
        uint32_t param[16];
        memset(param, 0, sizeof param);
        /* BT address of the 8BitDo: E4:17:D8:E0:A7:92 (little-endian) */
        unsigned char btaddr[6] = { 0x92, 0xA7, 0xE0, 0xD8, 0x17, 0xE4 };
        memcpy(param, btaddr, 6);
        param[2] = 0x3012;   /* 8BitDo PID */
        param[3] = 0x2dc8;   /* 8BitDo VID */
        param[4] = 0x002508; /* CoD: gamepad */
        r = reg_fn(param);
        log_ts("  Register(8BitDo): %#x", (unsigned)r);
        klog_poll();
    }

    /* pattern 3: a DualSense-like registration (to see the "right" answer) */
    {
        uint32_t param[16];
        memset(param, 0, sizeof param);
        unsigned char btaddr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
        memcpy(param, btaddr, 6);
        param[2] = 0x0ce6;   /* DualSense PID */
        param[3] = 0x054c;   /* Sony VID */
        param[4] = 0x002508;
        r = reg_fn(param);
        log_ts("  Register(fake DualSense): %#x", (unsigned)r);
        klog_poll();
    }

    /* pattern 4: just a size field (like AddDevice) */
    {
        uint32_t param[8];
        memset(param, 0xFF, sizeof param);
        param[0] = 32;       /* size */
        r = reg_fn(param);
        log_ts("  Register(size=32): %#x out: %08x %08x %08x %08x",
               (unsigned)r, param[1], param[2], param[3], param[4]);
    }

    /* watch the system for a while */
    log_ts("-- watching system for 10 seconds...");
    for (int i = 0; i < 200; i++) {
        klog_poll();
        usleep(50000);
    }

    /* also try the /dev/bt ioctls that returned 3 earlier */
    log_ts("-- /dev/bt ioctls that responded:");
    {
        int fd = open("/dev/bt", O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            unsigned long iocs[] = {
                0x8004622d, 0x8004622e, 0x8004622f, 0x80046230,
                0x80046231, 0x80046232, 0x80046233,
                0x8004623a, 0x8004623b, 0x8004623c,
            };
            unsigned char buf[256];
            for (int i = 0; i < (int)(sizeof iocs / sizeof iocs[0]); i++) {
                memset(buf, 0, sizeof buf);
                errno = 0;
                r = ioctl(fd, iocs[i], buf);
                log_ts("  ioctl %#lx: %d errno %d buf: %02x %02x %02x %02x",
                       iocs[i], r, errno, buf[0], buf[1], buf[2], buf[3]);
            }
            close(fd);
        }
    }

    log_ts("btreg: done");
    if (g_out) fclose(g_out);
    return 0;
}
