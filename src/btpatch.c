/* btpatch: finds the virtual DualSense's capability in kernel memory and
 * clears bit 31, making it indistinguishable from a real DualSense.
 *
 * Uses the SDK's built-in kernel read/write primitives:
 *   kernel_copyout(kaddr, buf, len) — read from kernel memory
 *   kernel_setint(addr, val)         — write to kernel memory
 *
 * The capability value 0x80003e37 is what a virtual DualSense reports.
 * A real DualSense reports ~0x00003e37 (without bit 31). Clearing that
 * bit makes the system treat the virtual as a native controller: full
 * LOGIN MGR, battery, vibration, triggers — everything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/stat.h>

/* SDK kernel functions — generic read/write to kernel memory */
#include <ps5/kernel.h>

/* pad functions */
int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t p);
int32_t scePadVirtualDeviceAddDevice(void *param, int32_t type);
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
int32_t scePadVirtualDeviceGetRemoteSetting(int32_t handle, void *out);
int32_t scePadGetCapability(int32_t handle, int32_t *capability);
int32_t sceUserServiceInitialize(void *p);

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

/* The target: virtual DualSense has capability 0x80003e37.
 * Real DualSense has ~0x00003e37. We scan for 0x80003e37 in kernel memory
 * and clear bit 31 at every location we find. */
#define VIRTUAL_CAP  0x80003e37u
#define REAL_CAP     (VIRTUAL_CAP & 0x7FFFFFFFu)

int main(void)
{
    int32_t param[8];
    int32_t vhandle = -1;
    int32_t vcap = -1;
    int r;

    mkdir(k_dir, 0777);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/btpatch.log", k_dir);
        g_out = fopen(path, "w");
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btpatch: making the virtual DualSense real to the kernel");

    /* elevate credentials */
    {
        static const uint8_t caps[16] = {
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        };
        pid_t me = getpid();
        if (kernel_set_ucred_authid(me, 0x3800000000010003ull) != 0 ||
            kernel_set_ucred_caps(me, caps) != 0) {
            log_ts("FATAL: cannot raise credentials");
            if (g_out) fclose(g_out);
            return 1;
        }
        log_ts("credentials raised");
    }

    /* init pad services */
    sceUserServiceInitialize(NULL);
    {
        void *mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
        if (!mbus) mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
        log_ts("libSceMbus: %s", mbus ? "loaded" : "not found");
    }
    scePadInit();
    scePadSetProcessPrivilege(1);

    /* create a virtual DualSense */
    log_ts("-- creating virtual DualSense:");
    memset(param, 0, sizeof param);
    param[0] = (int32_t)sizeof param;
    param[1] = 1;
    r = scePadVirtualDeviceAddDevice(param, 3);
    log_ts("  AddDevice: %#x", (unsigned)r);

    if (r != 0) {
        log_ts("  AddDevice failed; cannot proceed");
        if (g_out) fclose(g_out);
        return 1;
    }

    /* find the handle */
    {
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
    }

    if (vhandle < 0) {
        log_ts("  virtual handle not found by probing");
        if (g_out) fclose(g_out);
        return 1;
    }

    /* read the capability BEFORE the patch */
    scePadGetCapability(vhandle, &vcap);
    log_ts("  handle=%#x capability BEFORE=%#x", (unsigned)vhandle, (unsigned)vcap);

    if ((uint32_t)vcap != VIRTUAL_CAP) {
        log_ts("  unexpected capability (expected %#x, got %#x)",
               (unsigned)VIRTUAL_CAP, (unsigned)vcap);
        /* continue anyway: scan for whatever we have */
    }

    /* scan kernel memory for the capability value */
    log_ts("-- scanning kernel for %#x...", (unsigned)VIRTUAL_CAP);
    {
        /* the kernel data section is at KERNEL_ADDRESS_DATA_BASE */
        /* scan a wider range: the capability might be in a heap allocation */
        uint32_t target = (uint32_t)vcap;  /* use the actual value we read */
        uint32_t buf[16384];   /* 64 KB chunks */
        intptr_t start = KERNEL_ADDRESS_DATA_BASE;
        intptr_t end = start + 0x100000;    /* 1 MB of data section */
        int patched = 0;

        for (intptr_t addr = start; addr < end; addr += (intptr_t)sizeof buf - 4) {
            size_t to_read = sizeof buf;
            if ((intptr_t)(addr + (intptr_t)to_read) > end) to_read = (size_t)(end - addr);

            r = kernel_copyout(addr, buf, to_read);
            if (r != 0) {
                /* unreadable: skip to next page */
                addr = (addr + 0x1000) & ~0xFFFULL;
                addr -= (intptr_t)sizeof buf - 4;
                addr -= 1;
                continue;
            }

            /* search for the capability value */
            for (size_t i = 0; i < to_read / 4; i++) {
                if (buf[i] == target) {
                    intptr_t found = addr + (intptr_t)(i * 4);
                    log_ts("  FOUND at kernel address %#llx",
                           (unsigned long long)found);

                    /* read the surrounding context */
                    {
                        uint32_t ctx[8];
                        kernel_copyout(found - 16, ctx, sizeof ctx);
                        log_ts("    context: %08x %08x %08x %08x [%08x] %08x %08x %08x",
                               ctx[0], ctx[1], ctx[2], ctx[3],
                               ctx[4], ctx[5], ctx[6], ctx[7]);
                    }

                    /* PATCH: clear bit 31 */
                    {
                        uint32_t newval = target & 0x7FFFFFFFu;
                        r = kernel_setint(found, newval);
                        if (r == 0) {
                            log_ts("    PATCHED: %#x → %#x (%s)",
                                   (unsigned)target, (unsigned)newval,
                                   "bit 31 cleared");
                            patched++;
                        } else {
                            log_ts("    patch FAILED: kernel_setint errno %d", errno);
                        }
                    }
                }
            }
        }

        /* also scan the kernel text (rodata might have tables) */
        /* but text is usually read-only; skip for now */

        log_ts("  patched %d location(s)", patched);

        if (patched > 0) {
            /* verify: re-read the capability */
            usleep(100000);   /* let the system settle */
            scePadGetCapability(vhandle, &vcap);
            log_ts("  capability AFTER=%#x %s",
                   (unsigned)vcap,
                   (uint32_t)vcap == REAL_CAP ? "✓ REAL!" :
                   (uint32_t)vcap == VIRTUAL_CAP ? "(unchanged)" : "(changed)");
        }
    }

    /* keep the virtual alive so we can test it */
    log_ts("-- virtual is alive; press buttons to test");
    log_ts("(delete it with the AnyPad page when done)");

    /* run for 60 seconds watching for state changes */
    for (int i = 0; i < 600; i++) {
        usleep(100000);
        if (i % 100 == 0) {
            scePadGetCapability(vhandle, &vcap);
            log_ts("  %ds: capability=%#x", i / 10, (unsigned)vcap);
        }
    }

    log_ts("btpatch: done (virtual left alive for testing)");
    if (g_out) fclose(g_out);
    return 0;
}
