/* btglitch: makes the kernel see our controller as a real DualSense.
 *
 * Phase A (this tool, discovery): with a real DualSense connected and a
 * virtual one created, find both device structures in kernel memory and
 * dump them for byte-by-byte comparison. The difference tells us exactly
 * what to patch to make a virtual indistinguishable from real.
 *
 * Method:
 *   1. Elevate credentials (authid + caps)
 *   2. Create a virtual DualSense (AddDevice)
 *   3. Identify it (klog or probing)
 *   4. Scan kernel memory for DeviceId values (both real and virtual)
 *   5. Dump the containing structures
 *   6. Save everything to /data/btfix-out/
 *
 * Kernel access methods (in order):
 *   a. /dev/kmem (FreeBSD kernel memory device)
 *   b. /dev/mem (physical memory)
 *   c. kernel_read primitive from kstuff (if available)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

/* kernel credential functions from the SDK */
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

/* pad functions */
int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t p);
int32_t scePadVirtualDeviceAddDevice(void *param, int32_t type);
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
int32_t scePadVirtualDeviceGetRemoteSetting(int32_t handle, void *out);
int32_t scePadGetDeviceId(int32_t handle, int64_t *id);
int32_t sceUserServiceInitialize(void *p);

/* the kernel base address (from the Byepervisor dump) */
#define KERNEL_TEXT_BASE  0xFFFFFFFFD67C0000ULL
#define KERNEL_DATA_BASE  0xFFFFFFFFD7480000ULL
#define KERNEL_TEXT_SIZE  0x1340000   /* ~19.7 MB */
#define KERNEL_DATA_SIZE  0x40000     /* 256 KB */

static FILE *g_out;
static const char *k_dir = "/data/btfix-out";

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[%ld] ", now_ms());
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    fprintf(g_out ? g_out : stdout, "%s\n", buf);
    fprintf(stdout, "%s\n", buf);
    if (g_out) fflush(g_out);
}

/* ---- credential elevation ----------------------------------------------- */

static int creds_raise(void)
{
    static const uint8_t caps[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    pid_t me = getpid();
    uint8_t now[16];

    if (kernel_set_ucred_authid(me, 0x3800000000010003ull) != 0) return 0;
    if (kernel_set_ucred_caps(me, caps) != 0) return 0;
    if (kernel_get_ucred_authid(me) != 0x3800000000010003ull) return 0;
    if (kernel_get_ucred_caps(me, now) != 0 || memcmp(now, caps, 16) != 0) return 0;
    return 1;
}

/* ---- kernel memory access ------------------------------------------------ */

static int g_kmem = -1;

static int kmem_open(void)
{
    /* Try /dev/kmem (FreeBSD kernel virtual memory) */
    g_kmem = open("/dev/kmem", O_RDWR);
    if (g_kmem >= 0) {
        log_ts("kmem: /dev/kmem opened (fd %d)", g_kmem);
        return 1;
    }
    log_ts("kmem: /dev/kmem errno %d", errno);

    /* Try /dev/mem (physical memory) */
    g_kmem = open("/dev/mem", O_RDWR);
    if (g_kmem >= 0) {
        log_ts("kmem: /dev/mem opened (fd %d) (physical, need VA→PA)");
        return 2;  /* different: physical addresses, not virtual */
    }
    log_ts("kmem: /dev/mem errno %d", errno);

    return 0;
}

static long kmem_read(uint64_t addr, void *buf, size_t len)
{
    if (g_kmem < 0) return -1;
    if (lseek(g_kmem, (off_t)addr, SEEK_SET) == (off_t)-1) {
        log_ts("kmem: lseek to %#llx errno %d", (unsigned long long)addr, errno);
        return -1;
    }
    return read(g_kmem, buf, len);
}

/* Search kernel memory for a pattern, return the address */
static uint64_t kmem_search(uint64_t start, uint64_t end,
                              const void *pattern, size_t pat_len)
{
    unsigned char buf[65536];
    uint64_t addr;

    if (g_kmem < 0) return 0;

    for (addr = start; addr < end; addr += sizeof buf - pat_len) {
        size_t to_read = sizeof buf;
        if (addr + to_read > end) to_read = (size_t)(end - addr);
        if (kmem_read(addr, buf, to_read) != (long)to_read) {
            /* unreadable page: skip to the next page */
            addr = (addr + 0x1000) & ~0xFFFULL;
            addr -= sizeof buf - pat_len; /* will be re-added in the loop */
            addr -= 1; /* compensate for addr += ... */
            continue;
        }
        /* search this buffer */
        for (size_t i = 0; i + pat_len <= to_read; i++) {
            if (memcmp(buf + i, pattern, pat_len) == 0)
                return addr + i;
        }
    }
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    int32_t param[8];
    int32_t vhandle = -1;
    int64_t real_id = 0, virt_id = 0;
    int r;

    mkdir(k_dir, 0777);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/btglitch.log", k_dir);
        g_out = fopen(path, "w");
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btglitch: finding what makes a DualSense real to the kernel");

    /* elevate */
    if (!creds_raise()) {
        log_ts("FATAL: cannot raise credentials");
        if (g_out) fclose(g_out);
        return 1;
    }
    log_ts("credentials raised");

    /* init pad services */
    sceUserServiceInitialize(NULL);
    scePadInit();
    scePadSetProcessPrivilege(1);

    /* create a virtual DualSense */
    log_ts("-- creating virtual DualSense:");
    memset(param, 0, sizeof param);
    param[0] = (int32_t)sizeof param;
    param[1] = 1;
    r = scePadVirtualDeviceAddDevice(param, 3);   /* 3 = DualSense */
    log_ts("  AddDevice: %#x", (unsigned)r);
    if (r == 0) {
        /* find the handle via probing */
        static const int32_t candidates[] = {
            0x10301, 0x20301, 0x30301, 0x40301, 0x50301,
            0x60301, 0x70301, 0x80301, 0x90301, 0xA0301,
            0xB0301, 0xC0301, 0xD0301, 0xE0301, 0xF0301,
        };
        for (int i = 0; i < 15; i++) {
            unsigned char buf[128];
            if (scePadVirtualDeviceGetRemoteSetting(candidates[i], buf) == 0) {
                vhandle = candidates[i];
                break;
            }
        }
        if (vhandle >= 0) {
            log_ts("  virtual handle: %#x", (unsigned)vhandle);
            scePadGetDeviceId(vhandle, &virt_id);
            log_ts("  virtual deviceId: %#llx", (unsigned long long)virt_id);
        } else {
            log_ts("  virtual not found by probing");
        }
    }

    /* find a real DualSense (if connected) */
    log_ts("-- looking for real DualSense:");
    {
        /* probe a few known device IDs */
        /* Real DualSense typically have IDs like 0x30300, 0x50300, 0x50301 */
        static const int64_t real_candidates[] = {
            0x30300, 0x30301, 0x50300, 0x50301,
            0x7030d, 0x8030c,  /* from our earlier logs */
        };
        for (int i = 0; i < 6; i++) {
            int64_t id = 0;
            /* scePadGetDeviceId works with a handle, not a device ID directly */
            /* try reading via GetRemoteSetting (should fail for real pads) */
            unsigned char buf[128];
            if (scePadVirtualDeviceGetRemoteSetting((int32_t)real_candidates[i], buf) != 0) {
                /* not a virtual → might be real! */
                real_id = real_candidates[i];
                log_ts("  real device candidate: %#llx (not virtual)",
                       (unsigned long long)real_id);
                break;
            }
        }
        if (!real_id) log_ts("  no real DualSense found (may not be connected)");
    }

    /* open kernel memory */
    log_ts("-- kernel memory access:");
    int kmem_type = kmem_open();

    if (kmem_type == 1) {
        /* /dev/kmem works! Search for DeviceId patterns */
        log_ts("  searching kernel for device structures...");

        /* search for the virtual device's ID */
        if (virt_id) {
            uint64_t pattern = virt_id;
            log_ts("  searching for virtual ID %#llx...", (unsigned long long)virt_id);
            uint64_t found = kmem_search(KERNEL_DATA_BASE,
                                           KERNEL_DATA_BASE + KERNEL_DATA_SIZE,
                                           &pattern, sizeof pattern);
            if (found) {
                log_ts("  FOUND at kernel address %#llx", (unsigned long long)found);
                /* dump surrounding structure (256 bytes before and after) */
                unsigned char buf[512];
                uint64_t start = found - 256;
                if (kmem_read(start, buf, sizeof buf) == (long)sizeof buf) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/virt_struct.bin", k_dir);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(buf, 1, sizeof buf, f); fclose(f); }
                    log_ts("  dumped virt_struct.bin (%d bytes around %#llx)",
                           (int)sizeof buf, (unsigned long long)found);
                }
            } else {
                log_ts("  not found in data section, trying text...");
                found = kmem_search(KERNEL_TEXT_BASE,
                                     KERNEL_TEXT_BASE + KERNEL_TEXT_SIZE,
                                     &pattern, sizeof pattern);
                if (found) log_ts("  FOUND in text at %#llx",
                                   (unsigned long long)found);
                else log_ts("  NOT FOUND");
            }
        }

        /* search for the real device's ID */
        if (real_id) {
            uint64_t pattern = real_id;
            log_ts("  searching for real ID %#llx...", (unsigned long long)real_id);
            uint64_t found = kmem_search(KERNEL_DATA_BASE,
                                           KERNEL_DATA_BASE + KERNEL_DATA_SIZE,
                                           &pattern, sizeof pattern);
            if (found) {
                log_ts("  FOUND at kernel address %#llx", (unsigned long long)found);
                unsigned char buf[512];
                uint64_t start = found - 256;
                if (kmem_read(start, buf, sizeof buf) == (long)sizeof buf) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/real_struct.bin", k_dir);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(buf, 1, sizeof buf, f); fclose(f); }
                    log_ts("  dumped real_struct.bin");
                }
            } else {
                log_ts("  not found");
            }
        }
    } else if (kmem_type == 2) {
        log_ts("  /dev/mem available but uses physical addresses");
        log_ts("  (need VA→PA translation; deferred to Byepervisor)");
    } else {
        log_ts("  no kernel memory access from userland");
        log_ts("  (use Byepervisor to capture a live dump instead)");
        log_ts("  addresses to look for:");
        if (virt_id) log_ts("    virtual deviceId: %#llx", (unsigned long long)virt_id);
        if (real_id) log_ts("    real deviceId:    %#llx", (unsigned long long)real_id);
        log_ts("    kernel data base:  %#llx", KERNEL_DATA_BASE);
        log_ts("    kernel text base:  %#llx", KERNEL_TEXT_BASE);
    }

    /* cleanup */
    if (vhandle >= 0) {
        scePadVirtualDeviceDeleteDevice(vhandle);
        log_ts("virtual deleted");
    }

    log_ts("btglitch: done");
    if (g_out) fclose(g_out);
    return 0;
}
