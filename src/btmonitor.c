/* btmonitor: watches the PS5's own Bluetooth at work, without touching the
 * chip. A real DualSense connecting is captured from three sides at once:
 *
 *   1. /dev/bt  - bthid's notifications (the system's own view of its pads)
 *   2. the kernel log - what the system prints as it works
 *   3. the chip's two HCI functions - sampled with ONE read each, to see
 *      on which side each event flows (IF0 = ours, IF3 = the system's)
 *
 * Nothing is sent to the chip. No commands, no writes. The single event
 * read per function is armed only in short windows (200 ms every second),
 * to see the flow without stealing from the system's driver.
 *
 * Build: make ps5 with BTMON=1, or: prospero-clang ... -o btmonitor.elf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* ---- the console's notification device (bthid) ------------------------- */

static int g_bt = -1;

static FILE *g_out;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    int n;

    n = snprintf(buf, sizeof buf, "[%ld] ", now_ms());
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (g_out) {
        fprintf(g_out, "%s\n", buf);
        fflush(g_out);
    }
    printf("%s\n", buf);
}

/* ---- the kernel log (klog server on 3232) ------------------------------ */

static int g_klog = -1;
static char g_line[1024];
static size_t g_line_len;

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
    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    return fd;
}

static void klog_poll(void)
{
    char buf[2048];
    ssize_t n;
    int rounds;

    for (rounds = 0; rounds < 4 && g_klog >= 0; rounds++) {
        n = read(g_klog, buf, sizeof buf);
        if (n <= 0) return;
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n' && g_line_len < sizeof g_line - 1) {
                g_line[g_line_len++] = buf[i];
                continue;
            }
            g_line[g_line_len] = '\0';
            g_line_len = 0;
            /* keep what may speak of Bluetooth, pads, USB, mbus */
            if (strstr(g_line, "bt") || strstr(g_line, "Bt") || strstr(g_line, "BT") ||
                strstr(g_line, "luetooth") || strstr(g_line, "hci") || strstr(g_line, "HCI") ||
                strstr(g_line, "ugen") || strstr(g_line, "usb") || strstr(g_line, "USB") ||
                strstr(g_line, "DUALSENSE") || strstr(g_line, "ualSense") ||
                strstr(g_line, "mbus") || strstr(g_line, "MBUS") ||
                strstr(g_line, "LOGIN MGR") || strstr(g_line, "[Umm]") ||
                strstr(g_line, "pad") || strstr(g_line, "Pad") ||
                strstr(g_line, "controller") || strstr(g_line, "error") ||
                strstr(g_line, "SceBt") || strstr(g_line, "bthid"))
                log_ts("klog: %s", g_line);
        }
    }
}

/* ---- bthid's notifications ----------------------------------------------- */

static void bt_open(void)
{
    unsigned int reg[2] = { 0x044f, 0x1000 };
    int fd = open("/dev/bt", O_RDWR | O_NONBLOCK);

    if (fd < 0) {
        log_ts("bt: /dev/bt errno %d", errno);
        return;
    }
    if (ioctl(fd, 0x8004622b, reg) != 0) {
        log_ts("bt: register ioctl errno %d", errno);
        close(fd);
        return;
    }
    g_bt = fd;
    log_ts("bt: /dev/bt registered");
}

static void bt_poll(void)
{
    unsigned char ev[64];
    int n;

    if (g_bt < 0) return;
    while ((n = (int)read(g_bt, ev, sizeof ev)) > 0) {
        char hex[3 * 64 + 1];
        int i;
        for (i = 0; i < n && i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", ev[i]);
        hex[3 * (i < 32 ? i : 32)] = 0;
        log_ts("btdev: %d bytes: %s", n, hex);
    }
}

/* ---- the chip's event endpoints (sampled, never consumed) --------------- */

#define USB_FS_INIT        _IOWR('U', 1, struct usb_fs_init)
#define USB_FS_START       _IOWR('U', 2, struct usb_fs_start)
#define USB_FS_STOP        _IOWR('U', 3, struct usb_fs_stop)
#define USB_FS_COMPLETE    _IOWR('U', 4, struct usb_fs_complete)

struct usb_fs_endpoint {
    void *pBuffer;
    uint32_t bufferSize;
    uint32_t transferLength;
    uint32_t status;
    int32_t isError;
    uint8_t endpoint;
    uint8_t stream;
};
struct usb_fs_init {
    struct usb_fs_endpoint *pEndpoints;
    uint32_t ep_index_max;
};
struct usb_fs_start {
    uint8_t ep_index;
    uint16_t transferLength;
    uint32_t timeout;
};
struct usb_fs_stop { uint8_t ep_index; };
struct usb_fs_complete {
    uint8_t ep_index;
    void *pBuffer;
    uint32_t bufferLength;
    int32_t transferLength;
    int32_t isError;
    uint32_t status;
};

struct ugen_desc {
    const char *what;      /* "IF0 events" */
    uint8_t ep;            /* endpoint address */
    int idx;               /* transfer index */
    unsigned char buf[64];
    struct usb_fs_endpoint ed;
    int armed;
    long t_rearm;
};

static int g_usb = -1;
static struct ugen_desc g_eps[2];       /* the two event endpoints */
static struct usb_fs_endpoint g_ep_tbl[8];

static int usb_open(void)
{
    struct usb_fs_init init;
    int fd = open("/dev/ugen0.2", O_RDWR);

    if (fd < 0) {
        log_ts("usb: /dev/ugen0.2 errno %d", errno);
        return -1;
    }

    memset(g_eps, 0, sizeof g_eps);
    g_eps[0].what = "IF0 events (0x81)";
    g_eps[0].ep = 0x81;
    g_eps[0].idx = 0;
    g_eps[1].what = "IF3 events (0x8c)";
    g_eps[1].ep = 0x8c;
    g_eps[1].idx = 1;

    for (int i = 0; i < 2; i++) {
        g_ep_tbl[i].pBuffer = g_eps[i].buf;
        g_ep_tbl[i].bufferSize = 64;
        g_ep_tbl[i].endpoint = g_eps[i].ep;
    }
    memset(&init, 0, sizeof init);
    init.pEndpoints = g_ep_tbl;
    init.ep_index_max = 2;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        log_ts("usb: init errno %d", errno);
        close(fd);
        return -1;
    }
    log_ts("usb: opened, both event endpoints ready");
    return fd;
}

/* Arms one read for 200 ms, logs what it got, stops it again. */
static void usb_sample(int which)
{
    struct ugen_desc *d = &g_eps[which];
    struct usb_fs_start st;
    struct usb_fs_complete comp;
    long deadline;

    if (g_usb < 0) return;
    memset(&st, 0, sizeof st);
    st.ep_index = (uint8_t)d->idx;
    st.transferLength = 64;
    st.timeout = 200;                  /* ms */
    if (ioctl(g_usb, USB_FS_START, &st) != 0) return;

    deadline = now_ms() + 250;
    while (now_ms() < deadline) usleep(10000);

    memset(&comp, 0, sizeof comp);
    comp.ep_index = (uint8_t)d->idx;
    comp.pBuffer = d->buf;
    comp.bufferLength = 64;
    if (ioctl(g_usb, USB_FS_COMPLETE, &comp) == 0 && comp.transferLength > 0) {
        char hex[3 * 64 + 1];
        int i, n = comp.transferLength < 64 ? comp.transferLength : 64;
        for (i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", d->buf[i]);
        hex[3 * i] = 0;
        log_ts("hci %s: %d bytes: %s", d->what, comp.transferLength, hex);
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    long t_sample0 = 0, t_sample1 = 0;
    long t_klog_try = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_out = fopen("/data/btmonitor.log", "w");
    log_ts("btmonitor: watching the console's Bluetooth (no commands sent)");

    g_usb = usb_open();
    bt_open();

    for (;;) {
        long now = now_ms();

        /* the kernel log: opened once, polled every loop */
        if (g_klog < 0 && now >= t_klog_try) {
            g_klog = klog_open();
            t_klog_try = now + 10000;
            if (g_klog >= 0) log_ts("klog: source open");
        }
        if (g_klog >= 0) klog_poll();

        /* bthid: polled every loop */
        bt_poll();

        /* the chip's events: sampled once a second each, 200 ms window */
        if (g_usb >= 0 && now >= t_sample0) {
            usb_sample(0);
            t_sample0 = now + 1000;
        }
        if (g_usb >= 0 && now >= t_sample1) {
            usb_sample(1);
            t_sample1 = now + 1000;
        }

        usleep(50000);                /* 50 ms */
    }
    return 0;
}
