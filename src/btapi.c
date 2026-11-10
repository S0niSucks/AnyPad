/* btapi: finds and enumerates Sony's Bluetooth management APIs.
 *
 * libSceBluetoothHid is the library that manages BT HID devices on the PS5.
 * If we can call its connection functions, the 8BitDo connects through the
 * system's own stack - natively, with zero coexistence issues.
 *
 * This probe:
 *   1. Tries to load every BT-related .sprx we can guess at
 *   2. Enumerates all exports (by walking the symbol table, or known names)
 *   3. Reports everything found
 *
 * Output: /data/btapi.log and stdout.
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
#include <link.h>

static FILE *g_out;

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(g_out ? g_out : stdout, "%s\n", buf);
    fprintf(stdout, "%s\n", buf);
    if (g_out) fflush(g_out);
}

/* Every BT-related library name we can guess at */
static const char *const k_libs[] = {
    "/system/common/lib/libSceBluetoothHid.sprx",
    "/system/common/lib/libSceBluetooth.sprx",
    "/system/common/lib/libSceBt.sprx",
    "/system/common/lib/libSceBthid.sprx",
    "/system/common/lib/libSceBthidControl.sprx",
    "libSceBluetoothHid.sprx",
    "libSceBluetooth.sprx",
    "libSceBt.sprx",
    "libSceBthid.sprx",
    NULL,
};

/* Known/guessed export names for BT connection management */
static const char *const k_syms[] = {
    /* connection management */
    "sceBtConnect",
    "sceBtDisconnect",
    "sceBtPair",
    "sceBtUnpair",
    "sceBtScan",
    "sceBtStartScan",
    "sceBtStopScan",
    "sceBtInquiry",
    "sceBtAccept",
    "sceBtReject",
    "sceBtDiscover",
    "sceBtFindDevice",
    "sceBtGetDeviceList",
    "sceBtSetDiscoverable",
    /* HID specific */
    "sceBthidConnect",
    "sceBthidDisconnect",
    "sceBthidPair",
    "sceBthidRegister",
    "sceBthidAddDevice",
    "sceBthidRemoveDevice",
    "sceBthidSetReport",
    "sceBthidGetReport",
    "sceBthidOpen",
    "sceBthidClose",
    "sceBthidInit",
    "sceBthidTerm",
    /* generic prefix */
    "sceBluetoothInit",
    "sceBluetoothTerm",
    "sceBluetoothConnect",
    "sceBluetoothDisconnect",
    "sceBluetoothPair",
    "sceBluetoothScan",
    "sceBluetoothGetDeviceList",
    "sceBluetoothHidInit",
    "sceBluetoothHidTerm",
    "sceBluetoothHidConnect",
    "sceBluetoothHidDisconnect",
    "sceBluetoothHidPair",
    /* remote play / virtual device paths */
    "sceBluetoothHidRegisterVirtualDevice",
    "sceBluetoothHidAddVirtualDevice",
    "sceBluetoothHidCreateVirtualDevice",
    /* ioctl-style */
    "sceBtIoctl",
    "sceBthidIoctl",
    /* others */
    "sceBtGetVersion",
    "sceBtGetStatus",
    "sceBtGetConnectionStatus",
    "sceBtGetDeviceInfo",
    "sceBtSetDeviceProperty",
    "sceBthidGetConnectionInfo",
    "sceBthidSetConnectionCallback",
    NULL,
};

/* Walk the dynamic symbol table of a loaded object */
static int enumerate_exports(void *handle, const char *libname)
{
    /* Use dl_iterate_phdr to find the object and walk its .dynsym */
    /* For now, use dlsym with the known names */
    int found = 0;

    log_ts("-- exports of %s:", libname);
    for (const char *const *s = k_syms; *s; s++) {
        void *p = dlsym(handle, *s);
        if (p) {
            log_ts("  FOUND: %s at %p", *s, p);
            found++;
        }
    }

    if (!found) log_ts("  (no known exports found)");
    return found;
}

int main(void)
{
    g_out = fopen("/data/btfix-out/btapi.log", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btapi: finding Sony's Bluetooth management APIs");

    /* try each library */
    for (const char *const *lib = k_libs; *lib; lib++) {
        void *h = dlopen(*lib, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            log_ts("LOADED: %s", *lib);
            enumerate_exports(h, *lib);
            /* don't dlclose: we might need it later */
        } else {
            log_ts("  (not found: %s)", *lib);
        }
    }

    /* also check what's already loaded in our process (from other libs) */
    log_ts("-- checking already-loaded symbols:");
    for (const char *const *s = k_syms; *s; s++) {
        void *p = dlsym(RTLD_DEFAULT, *s);
        if (p) {
            log_ts("  FOUND (loaded): %s at %p", *s, p);
        }
    }

    /* check /dev for BT-related devices */
    log_ts("-- /dev entries (bt-related):");
    {
        const char *devs[] = {
            "/dev/bt", "/dev/bthid", "/dev/bluetooth",
            "/dev/ugen0.2", "/dev/ugen0.3",
            NULL,
        };
        for (const char *const *d = devs; *d; d++) {
            int fd = open(*d, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                log_ts("  OPEN: %s (fd %d)", *d, fd);
                close(fd);
            } else {
                log_ts("  (cannot open %s: errno %d)", *d, errno);
            }
        }
    }

    /* check /system/common/lib for any BT-related files */
    log_ts("-- /system/common/lib contents (BT/hid filtered):");
    {
        /* we can't opendir from a payload easily, but let's try */
        /* the system path might need special privileges */
        log_ts("  (check manually: ls /system/common/lib/ | grep -i 'bt\\|blue\\|hid')");
    }

    log_ts("btapi: done");
    if (g_out) fclose(g_out);
    return 0;
}
