/* btdisasm: reads the decrypted code of libSceBluetoothHid's functions
 * from memory (the library is loaded by this process), dumps the raw bytes
 * for offline disassembly, and also tries GetDeviceInfo/UnregisterDevice.
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

/* Dump memory around an address */
static void dump_mem(const char *name, uintptr_t addr, int before, int after)
{
    FILE *f;
    char path[256];
    unsigned char *p = (unsigned char *)addr;
    int total = before + after;

    snprintf(path, sizeof path, "/data/btdisasm_%s.bin", name);
    f = fopen(path, "wb");
    if (!f) {
        log_ts("  (cannot write %s: errno %d)", path, errno);
        return;
    }
    fwrite(p - before, 1, total, f);
    fclose(f);
    log_ts("  dumped %s: %p ± %d bytes (%d total) → %s",
           name, (void *)addr, before, total, path);
}

int main(void)
{
    void *lib;
    void *init_fn, *reg_fn, *unreg_fn, *info_fn;

    g_out = fopen("/data/btdisasm.log", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("btdisasm: reading libSceBluetoothHid's decrypted code");

    lib = dlopen("/system/common/lib/libSceBluetoothHid.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        log_ts("FATAL: cannot load: %s", dlerror());
        if (g_out) fclose(g_out);
        return 1;
    }

    /* find all four functions */
    init_fn  = dlsym(lib, "sceBluetoothHidInit");
    reg_fn   = dlsym(lib, "sceBluetoothHidRegisterDevice");
    unreg_fn = dlsym(lib, "sceBluetoothHidUnregisterDevice");
    info_fn  = dlsym(lib, "sceBluetoothHidGetDeviceInfo");

    log_ts("-- function addresses:");
    log_ts("  Init:           %p", init_fn);
    log_ts("  RegisterDevice: %p", reg_fn);
    log_ts("  UnregisterDev:  %p", unreg_fn);
    log_ts("  GetDeviceInfo:  %p", info_fn);

    /* dump the code: 256 bytes before (possible header/prologue context)
     * and 1024 bytes after (the function body) */
    log_ts("-- dumping code for offline disassembly:");
    if (init_fn)  dump_mem("init",  (uintptr_t)init_fn,  64, 512);
    if (reg_fn)   dump_mem("reg",   (uintptr_t)reg_fn,   64, 1024);
    if (unreg_fn) dump_mem("unreg", (uintptr_t)unreg_fn, 64, 512);
    if (info_fn)  dump_mem("info",  (uintptr_t)info_fn,  64, 1024);

    /* also dump the ELF header of the loaded library to find the full
     * export table */
    log_ts("-- dumping library headers:");
    {
        /* the base is before the first function; try to find the ELF magic */
        uintptr_t base = (uintptr_t)init_fn & ~0xFFFULL;   /* page-align down */
        for (int i = 0; i < 64; i++) {
            unsigned char *p = (unsigned char *)(base - i * 0x1000);
            if (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') {
                log_ts("  ELF header found at %p (offset %#lx from Init)",
                       (void *)p, (uintptr_t)init_fn - (uintptr_t)p);
                dump_mem("elfhdr", (uintptr_t)p, 0, 4096);
                break;
            }
            /* also check if the page is readable */
            if (i > 0) {
                /* we can't easily check readability; just try */
            }
        }
    }

    /* try calling GetDeviceInfo with various patterns */
    log_ts("-- calling sceBluetoothHidGetDeviceInfo:");
    if (info_fn) {
        int32_t (*fn)(void *, void *) = (int32_t (*)(void *, void *))info_fn;
        uint32_t param_in[16], param_out[16];
        int r;

        /* pattern 1: NULL, NULL */
        r = fn(NULL, NULL);
        log_ts("  GetDeviceInfo(NULL, NULL): %#x", (unsigned)r);

        /* pattern 2: index 0, output buffer */
        memset(param_out, 0xFF, sizeof param_out);
        r = fn((void *)(intptr_t)0, param_out);
        log_ts("  GetDeviceInfo(0, out): %#x out: %08x %08x %08x %08x",
               (unsigned)r, param_out[0], param_out[1], param_out[2], param_out[3]);

        /* pattern 3: index 1 */
        memset(param_out, 0xFF, sizeof param_out);
        r = fn((void *)(intptr_t)1, param_out);
        log_ts("  GetDeviceInfo(1, out): %#x out: %08x %08x %08x %08x",
               (unsigned)r, param_out[0], param_out[1], param_out[2], param_out[3]);

        /* pattern 4: a struct with size */
        memset(param_in, 0, sizeof param_in);
        param_in[0] = 64;   /* size */
        memset(param_out, 0xFF, sizeof param_out);
        r = fn(param_in, param_out);
        log_ts("  GetDeviceInfo(size=64): %#x out: %08x %08x %08x %08x",
               (unsigned)r, param_out[0], param_out[1], param_out[2], param_out[3]);
    }

    /* try calling UnregisterDevice */
    log_ts("-- calling sceBluetoothHidUnregisterDevice:");
    if (unreg_fn) {
        int32_t (*fn)(void *) = (int32_t (*)(void *))unreg_fn;
        int r;

        r = fn(NULL);
        log_ts("  Unregister(NULL): %#x", (unsigned)r);

        {
            uint32_t param[8];
            memset(param, 0, sizeof param);
            r = fn(param);
            log_ts("  Unregister(zeros): %#x", (unsigned)r);
        }
    }

    /* also try RegisterDevice with a two-argument call (maybe it takes
     * (struct in, struct out) like AddDevice */
    log_ts("-- RegisterDevice with two arguments:");
    if (reg_fn) {
        int32_t (*fn)(void *, void *) = (int32_t (*)(void *, void *))reg_fn;
        uint32_t in[16], out[16];
        int r;

        memset(in, 0, sizeof in);
        memset(out, 0xFF, sizeof out);
        r = fn(in, out);
        log_ts("  Register(zeros, out): %#x out: %08x %08x",
               (unsigned)r, out[0], out[1]);

        /* try with a BT address as the second arg */
        in[0] = 32;   /* size */
        r = fn(in, out);
        log_ts("  Register(size=32, out): %#x out: %08x %08x",
               (unsigned)r, out[0], out[1]);
    }

    log_ts("btdisasm: done");
    if (g_out) fclose(g_out);
    return 0;
}
