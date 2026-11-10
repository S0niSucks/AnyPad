/* dualsim: the proof of concept for the DualSense simulator.
 *
 * What this tests: whether the two HCI functions of the MediaTek chip
 * are independent hosts or two views of the same controller.
 *
 *   Phase A: 8BitDo pages the console. We DON'T accept on IF0.
 *            We watch IF0's event endpoint (1 read) and the kernel log.
 *            If the system (on IF3) accepts it, we know the system CAN
 *            see and handle third-party controllers.
 *
 *   Phase B: We create a connection on IF0 (to the 8BitDo, as AnyPad does).
 *            We watch IF3's event endpoint (1 read) to see if the
 *            Connection Complete appears there too. If yes: the chip
 *            mirrors events, and the simulator architecture works.
 *
 * Output: /data/dualsim.log and stdout.
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

/* ---- USB constants (from usb_desc.h) ----------------------------------- */

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

#define USB_FS_INIT     _IOWR('U', 1, struct usb_fs_init)
#define USB_FS_START    _IOWR('U', 2, struct usb_fs_start)
#define USB_FS_STOP     _IOWR('U', 3, struct usb_fs_stop)
#define USB_FS_COMPLETE _IOWR('U', 4, struct usb_fs_complete)

/* ---- logging ----------------------------------------------------------- */

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
    if (g_out) { fprintf(g_out, "%s\n", buf); fflush(g_out); }
    printf("%s\n", buf);
}

/* ---- klog (the system's own prints) ------------------------------------- */

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

static int klog_interesting(const char *s)
{
    return strstr(s, "luetooth") || strstr(s, "hci") || strstr(s, "HCI") ||
           strstr(s, "ugen") || strstr(s, "DUALSENSE") || strstr(s, "ualSense") ||
           strstr(s, "mbus") || strstr(s, "MBUS") || strstr(s, "LOGIN MGR") ||
           strstr(s, "[Umm]") || strstr(s, "bthid") || strstr(s, "SceBt") ||
           strstr(s, "pad") || strstr(s, "Pad") || strstr(s, "device") ||
           strstr(s, "error");
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
            if (buf[i] != '\n' && g_kline_len < sizeof g_kline - 1) {
                g_kline[g_kline_len++] = buf[i];
                continue;
            }
            g_kline[g_kline_len] = '\0';
            g_kline_len = 0;
            if (klog_interesting(g_kline))
                log_ts("sys: %s", g_kline);
        }
    }
}

/* ---- the chip: open, sample events, send a command ---------------------- */

static int g_usb = -1;

/* Two event endpoints: IF0's 0x81 and IF3's 0x8c */
#define N_EP  2
static struct {
    const char *name;
    uint8_t ep;
    unsigned char buf[64];
} g_eps[N_EP] = {
    { "IF0 (0x81)", 0x81 },
    { "IF3 (0x8c)", 0x8c },
};
static struct usb_fs_endpoint g_tbl[N_EP];

static int usb_open(void)
{
    struct usb_fs_init init;
    int fd = open("/dev/ugen0.2", O_RDWR);

    if (fd < 0) {
        log_ts("usb: /dev/ugen0.2 errno %d", errno);
        return -1;
    }
    for (int i = 0; i < N_EP; i++) {
        g_tbl[i].pBuffer = g_eps[i].buf;
        g_tbl[i].bufferSize = 64;
        g_tbl[i].endpoint = g_eps[i].ep;
    }
    memset(&init, 0, sizeof init);
    init.pEndpoints = g_tbl;
    init.ep_index_max = N_EP;
    if (ioctl(fd, USB_FS_INIT, &init) != 0) {
        log_ts("usb: init errno %d", errno);
        close(fd);
        return -1;
    }
    log_ts("usb: opened, IF0 events 0x81 + IF3 events 0x8c ready");
    return fd;
}

/* Arms one read for ms milliseconds, logs whatever arrives. */
static int sample(int which, int ms)
{
    struct usb_fs_start st;
    struct usb_fs_complete comp;

    if (g_usb < 0) return -1;
    memset(&st, 0, sizeof st);
    st.ep_index = (uint8_t)which;
    st.transferLength = 64;
    st.timeout = (uint32_t)ms;
    if (ioctl(g_usb, USB_FS_START, &st) != 0) return -1;

    usleep(ms * 1000);

    memset(&comp, 0, sizeof comp);
    comp.ep_index = (uint8_t)which;
    comp.pBuffer = g_eps[which].buf;
    comp.bufferLength = 64;
    if (ioctl(g_usb, USB_FS_COMPLETE, &comp) == 0 && comp.transferLength > 0) {
        char hex[3 * 64 + 1];
        int i, n = comp.transferLength < 64 ? comp.transferLength : 64;
        for (i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", g_eps[which].buf[i]);
        hex[3 * i] = 0;
        log_ts("EVENT %s: %d bytes: %s", g_eps[which].name, comp.transferLength, hex);
        return n;
    }
    return 0;
}

/* Sends an HCI command via control transfer to interface 0. */
static int hci_cmd(unsigned opcode, const void *params, int plen)
{
    /* USB control transfer: bmRequestType=0x20 (host-to-device, class,
     * interface), bRequest=0 (HCI command), wValue=0, wIndex=iface 0 */
    unsigned char setup[8];
    unsigned char data[256];

    setup[0] = 0x20;
    setup[1] = 0x00;
    setup[2] = (unsigned char)(opcode & 0xFF);
    setup[3] = (unsigned char)(opcode >> 8);
    setup[4] = 0x00;  /* wValue low */
    setup[5] = 0x00;  /* wValue high */
    setup[6] = 0x00;  /* wIndex = interface 0 */
    setup[7] = 0x00;

    data[0] = (unsigned char)(opcode & 0xFF);
    data[1] = (unsigned char)(opcode >> 8);
    data[2] = (unsigned char)plen;
    if (plen > 0 && plen <= 252) memcpy(data + 3, params, (size_t)plen);

    log_ts("CMD %04x (%d bytes)", opcode, plen);
    return 0;  /* actual send via USB control transfer */
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    long t_klog_try = 0;
    long t_sample = 0;
    long start = now_ms();
    int phase_b = argc > 1 && strcmp(argv[1], "b") == 0;

    g_out = fopen("/data/dualsim.log", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("dualsim: proof of concept for the DualSense simulator");
    log_ts("mode: %s", phase_b ? "B (we create, watch IF3)" : "A (8BitDo pages, we stand aside)");

    g_usb = usb_open();
    if (g_usb < 0) {
        log_ts("FATAL: cannot open the chip");
        if (g_out) fclose(g_out);
        return 1;
    }

    for (;;) {
        long now = now_ms();

        if (g_klog < 0 && now >= t_klog_try) {
            g_klog = klog_open();
            t_klog_try = now + 10000;
            if (g_klog >= 0) log_ts("klog: source open");
        }
        if (g_klog >= 0) klog_poll();

        /* sample both event endpoints once a second (500 ms each, 200 ms read) */
        if (now >= t_sample) {
            sample(0, 200);   /* IF0 */
            usleep(10000);
            sample(1, 200);   /* IF3 */
            t_sample = now + 1000;
        }

        /* phase A: run for 5 minutes, then stop */
        if (!phase_b && now - start > 300000) {
            log_ts("dualsim: 5 minutes done (phase A)");
            break;
        }

        /* phase B: after 10 seconds, create a connection to the 8BitDo */
        if (phase_b && now - start == 0) {
            /* nothing yet */
        }
        if (phase_b && now - start > 10000 && now - start < 10500) {
            log_ts("phase B: creating connection (8BitDo MAC from AnyPad's db)");
            /* TODO: actual HCI_Create_connection */
        }

        usleep(50000);
    }

    log_ts("dualsim: done");
    if (g_out) fclose(g_out);
    return 0;
}
