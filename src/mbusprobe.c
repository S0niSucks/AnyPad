/* mbusprobe: finds the injection point into the system's pad pipeline.
 *
 * What it does on the console:
 *   1. Enumerates every export of libSceMbus.sprx and libScePad.sprx
 *      (dlopen + walking the symbol table, or known names via dlsym)
 *   2. Creates a virtual DualSense, then asks the system everything it
 *      knows about it (GetCapability, GetDeviceInfo, GetBluetoothAddress,
 *      GetControllerInformation) - to see what "virtual" means to the system
 *   3. Tries different parameter values for scePadVirtualDeviceAddDevice,
 *      looking for one that makes the device report capabilityBattery:1
 *
 * Output goes to /data/mbusprobe.log and stdout.
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
#include <unistd.h>
#include <sys/types.h>

/* From the payload SDK (ps5/kernel.h). */
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

static uint64_t g_orig_authid;
static uint8_t g_orig_caps[16];
static int g_have_orig;
static int g_raised;

static int creds_raise(void)
{
    static const uint8_t caps[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    pid_t me = getpid();
    uint8_t now[16];

    if (!g_have_orig) {
        g_orig_authid = kernel_get_ucred_authid(me);
        if (!g_orig_authid || kernel_get_ucred_caps(me, g_orig_caps) != 0) return 0;
        g_have_orig = 1;
    }
    if (kernel_set_ucred_authid(me, 0x3800000000010003ull) != 0 ||
        kernel_set_ucred_caps(me, caps) != 0 ||
        kernel_get_ucred_authid(me) != 0x3800000000010003ull ||
        kernel_get_ucred_caps(me, now) != 0 || memcmp(now, caps, 16) != 0) return 0;
    g_raised = 1;
    return 1;
}

/* ---- logging ----------------------------------------------------------- */

static FILE *g_out;

static void log_ts(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    snprintf(buf, sizeof buf, "%s\n", fmt);
    /* no timestamps here: everything is fast */
    va_start(ap, fmt);
    vfprintf(g_out ? g_out : stdout, buf, ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, buf, ap);
    va_end(ap);
    if (g_out) fflush(g_out);
}

#define CHECK(c, msg) do { \
    int32_t _r = (int32_t)(c); \
    log_ts("  %s: %#x %s", msg, (unsigned)_r, _r == 0 ? "OK" : ""); \
} while (0)

/* ---- system declarations ------------------------------------------------ */

int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t privilege);
int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
int32_t scePadVirtualDeviceGetRemoteSetting(int32_t handle, void *out);
int32_t scePadGetCapability(int32_t handle, int32_t *capability);
int32_t scePadGetControllerInformation(int32_t handle, void *info);
int32_t scePadGetBluetoothAddress(int32_t handle, void *addr);
int32_t scePadGetDeviceId(int32_t handle, int64_t *deviceId);
int32_t scePadGetDeviceInfo(int32_t handle, void *info);
int32_t scePadGetHandle(int32_t user, int32_t type, int32_t index);
int32_t scePadOutputReport(int32_t handle, const void *report, int32_t len);
int32_t scePadClose(int32_t handle);
int32_t scePadMbusInit(void);
int32_t scePadMbusTerm(void);

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user);
int32_t sceUserServiceGetForegroundUser(int32_t *user);
int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]);

typedef int32_t (*mbus_bind_fn)(uint64_t device_id, int32_t user);

static void *g_mbus_lib;

/* ---- the probe ------------------------------------------------------------ */

/* Every name we know of or guess at in libSceMbus, tried with dlsym. */
static const char *const k_mbus_names[] = {
    "sceMbusBindDeviceWithUserId",
    "sceMbusUnbindDevice",
    "sceMbusDisconnectDevice",
    "sceMbusGetDeviceInfo",
    "sceMbusAddDevice",
    "sceMbusRemoveDevice",
    "sceMbusRegisterDevice",
    "sceMbusUnregisterDevice",
    "sceMbusCreateDevice",
    "sceMbusDeleteDevice",
    "sceMbusSetDeviceCapability",
    "sceMbusSetDeviceInfo",
    "sceMbusReportDevice",
    "sceMbusNotifyDevice",
    "sceMbusEnumerateDevice",
    "sceMbusGetDeviceList",
    "sceMbusInit",
    "sceMbusTerm",
    NULL,
};

/* Every name we guess at in libScePad, beyond what we already use. */
static const char *const k_pad_names[] = {
    "scePadMbusInit",
    "scePadMbusTerm",
    "scePadEnableAutoDetect",
    "scePadSwitchConnection",
    "scePadEnableUsbConnection",
    "scePadDeviceOpen",
    "scePadDisconnectDevice",
    "scePadDisconnectPort",
    "scePadGetBluetoothAddress",
    "scePadGetCapability",
    "scePadGetDeviceId",
    "scePadGetDeviceInfo",
    "scePadGetControllerInformation",
    "scePadGetExtControllerInformation",
    "scePadOutputReport",
    "scePadSetFeatureReport",
    "scePadGetFeatureReport",
    "scePadSetLoginUserNumber",
    "scePadSetUserNumber",
    "scePadSetUserColor",
    "scePadRemoteControlInsertData",
    "scePadVertualDeviceAddDevice",
    NULL,
};

int main(void)
{
    int32_t pad_handle = -1;
    int64_t device_id;
    unsigned char info[128];
    int32_t capability = -1;
    int32_t bt_addr[4];
    int r;

    g_out = fopen("/data/mbusprobe.log", "w");
    setvbuf(stdout, NULL, _IOLBF, 0);
    log_ts("mbusprobe: finding the injection point into the pad pipeline");

    /* raise credentials first: without them, MBus refuses everything */
    if (!creds_raise()) {
        log_ts("FATAL: could not raise credentials; run from the payload loader");
        if (g_out) fclose(g_out);
        return 1;
    }
    log_ts("credentials raised (authid 0x3800000000010003)");

    /* libSceMbus: loaded by libScePad's import, or by us */
    g_mbus_lib = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!g_mbus_lib) g_mbus_lib = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    log_ts("libSceMbus: %s", g_mbus_lib ? "loaded" : dlerror());

    if (g_mbus_lib) {
        log_ts("-- libSceMbus exports (known and guessed):");
        for (const char *const *n = k_mbus_names; *n; n++) {
            void *p = dlsym(g_mbus_lib, *n);
            if (p) log_ts("  FOUND: %s at %p", *n, p);
        }
    }

    log_ts("-- libScePad exports (beyond what we use):");
    for (const char *const *n = k_pad_names; *n; n++) {
        void *p = dlsym(RTLD_DEFAULT, *n);
        if (p) log_ts("  FOUND: %s at %p", *n, p);
    }

    /* Pad init */
    log_ts("-- user and pad init:");
    {
        int32_t init_user = -1, fore_user = -1, login[4] = { -1, -1, -1, -1 };
        CHECK(sceUserServiceInitialize(NULL), "UserServiceInitialize");
        CHECK(sceUserServiceGetInitialUser(&init_user), "GetInitialUser");
        CHECK(sceUserServiceGetForegroundUser(&fore_user), "GetForegroundUser");
        CHECK(sceUserServiceGetLoginUserIdList(login), "GetLoginUserIdList");
        log_ts("  initial=%#x, foreground=%#x, login: %#x %#x %#x %#x",
               (unsigned)init_user, (unsigned)fore_user,
               (unsigned)login[0], (unsigned)login[1],
               (unsigned)login[2], (unsigned)login[3]);
    }
    CHECK(scePadInit(), "scePadInit");
    CHECK(scePadSetProcessPrivilege(1), "scePadSetProcessPrivilege");
    CHECK(scePadMbusInit(), "scePadMbusInit");

    /* Create a virtual DualSense and interrogate it */
    log_ts("-- virtual DualSense, interrogated:");
    {
        int32_t param[8];
        int i;

        for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
        param[0] = (int32_t)sizeof param;
        param[1] = 1;
        r = scePadVirtualDeviceAddDevice(param, 3);   /* 3 = DualSense */
        log_ts("  AddDevice(param, 3): %#x", (unsigned)r);
        log_ts("  param out: %08x %08x %08x %08x %08x %08x",
               (unsigned)param[2], (unsigned)param[3], (unsigned)param[4],
               (unsigned)param[5], (unsigned)param[6], (unsigned)param[7]);

        if (r == 0) {
            int32_t handle = param[2];  /* or wherever the handle lands */

            /* what the system knows about it */
            memset(info, 0, sizeof info);
            CHECK(scePadGetCapability(handle, &capability), "GetCapability");
            log_ts("  capability: %#x", (unsigned)capability);

            memset(bt_addr, 0, sizeof bt_addr);
            CHECK(scePadGetBluetoothAddress(handle, bt_addr), "GetBluetoothAddress");
            log_ts("  BT addr: %08x %08x %08x %08x",
                   (unsigned)bt_addr[0], (unsigned)bt_addr[1],
                   (unsigned)bt_addr[2], (unsigned)bt_addr[3]);

            device_id = 0;
            CHECK(scePadGetDeviceId(handle, &device_id), "GetDeviceId");
            log_ts("  deviceId: %#llx", (unsigned long long)device_id);

            memset(info, 0, sizeof info);
            CHECK(scePadGetControllerInformation(handle, info), "GetControllerInformation");
            {
                /* first 32 bytes as hex */
                unsigned char *b = (unsigned char *)info;
                char hex[3 * 32 + 1];
                int k;
                for (k = 0; k < 32; k++) snprintf(hex + 3 * k, 4, "%02x ", b[k]);
                hex[3 * 32] = 0;
                log_ts("  controllerInfo: %s", hex);
            }

            memset(info, 0, sizeof info);
            CHECK(scePadGetDeviceInfo(handle, info), "GetDeviceInfo");
            {
                unsigned char *b = (unsigned char *)info;
                char hex[3 * 32 + 1];
                int k;
                for (k = 0; k < 32; k++) snprintf(hex + 3 * k, 4, "%02x ", b[k]);
                hex[3 * 32] = 0;
                log_ts("  deviceInfo: %s", hex);
            }

            /* try a raw output report (DualSense report 0x31: rumble) */
            {
                unsigned char rep[48] = { 0 };
                rep[0] = 0x31;     /* DualSense output report ID */
                rep[2] = 0x03;     /* valid flags: rumble */
                rep[3] = 0x0F;     /* more flags */
                rep[4] = 120;      /* weak rumble */
                rep[5] = 180;      /* strong rumble */
                CHECK(scePadOutputReport(handle, rep, sizeof rep), "OutputReport (rumble)");
            }

            /* the remote setting (the console's wishes, as we read in output_poll) */
            {
                unsigned char setting[128];
                memset(setting, 0, sizeof setting);
                CHECK(scePadVirtualDeviceGetRemoteSetting(handle, setting), "GetRemoteSetting");
                {
                    char hex[3 * 32 + 1];
                    int k;
                    for (k = 0; k < 32; k++) snprintf(hex + 3 * k, 4, "%02x ", setting[k]);
                    hex[3 * 32] = 0;
                    log_ts("  remoteSetting: %s", hex);
                }
            }

            scePadVirtualDeviceDeleteDevice(handle);
            log_ts("  (deleted)");
        }
    }

    /* try different param[1] values for AddDevice */
    log_ts("-- AddDevice with different param values:");
    {
        int32_t try_vals[] = { 0, 1, 2, 3, 4, 5, 0x100, 0x101, 0x102, 0x103 };
        int t;
        for (t = 0; t < (int)(sizeof try_vals / sizeof try_vals[0]); t++) {
            int32_t param[8];
            int i;
            for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
            param[0] = (int32_t)sizeof param;
            param[1] = try_vals[t];
            r = scePadVirtualDeviceAddDevice(param, 3);
            log_ts("  param1=%d: ret=%#x out=%08x %08x",
                   (int)try_vals[t], (unsigned)r,
                   (unsigned)param[2], (unsigned)param[3]);
            if (r == 0 && param[2] != (int32_t)0xDEADBEEF) {
                /* check capability of this one */
                int32_t cap = -1;
                scePadGetCapability(param[2], &cap);
                log_ts("    capability: %#x", (unsigned)cap);
                scePadVirtualDeviceDeleteDevice(param[2]);
            }
        }
    }

    /* try different deviceType values */
    log_ts("-- AddDevice with different device types:");
    {
        int32_t types[] = { 0, 1, 2, 3, 4, 5, 6, 7, 22 };
        int t;
        for (t = 0; t < (int)(sizeof types / sizeof types[0]); t++) {
            int32_t param[8];
            int i;
            for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
            param[0] = (int32_t)sizeof param;
            param[1] = 1;
            r = scePadVirtualDeviceAddDevice(param, types[t]);
            if (r == 0) {
                int32_t cap = -1;
                scePadGetCapability(param[2], &cap);
                log_ts("  type %d: OK, handle=%d, cap=%#x",
                       (int)types[t], (int)param[2], (unsigned)cap);
                scePadVirtualDeviceDeleteDevice(param[2]);
            } else {
                log_ts("  type %d: %#x", (int)types[t], (unsigned)r);
            }
        }
    }

    /* what a real pad looks like (if one is connected) */
    log_ts("-- real pad (if connected):");
    {
        int32_t init_user = -1, fore_user = -1;
        int32_t try_user;

        sceUserServiceGetForegroundUser(&fore_user);
        sceUserServiceGetInitialUser(&init_user);
        try_user = fore_user != -1 ? fore_user : init_user;

        if (try_user == -1) {
            log_ts("  no user to look up (initial=%#x foreground=%#x)",
                   (unsigned)init_user, (unsigned)fore_user);
        } else {
            pad_handle = scePadGetHandle(try_user, 0, 0);
            log_ts("  GetHandle(user %#x): %d", (unsigned)try_user, (int)pad_handle);
            if (pad_handle >= 0) {
                int32_t cap = -1;
                scePadGetCapability(pad_handle, &cap);
                log_ts("  handle %d: capability %#x", (int)pad_handle, (unsigned)cap);

                memset(bt_addr, 0, sizeof bt_addr);
                CHECK(scePadGetBluetoothAddress(pad_handle, bt_addr), "real GetBluetoothAddress");
                log_ts("  real BT addr: %08x %08x %08x %08x",
                       (unsigned)bt_addr[0], (unsigned)bt_addr[1],
                       (unsigned)bt_addr[2], (unsigned)bt_addr[3]);

                memset(info, 0, sizeof info);
                CHECK(scePadGetControllerInformation(pad_handle, info), "real GetControllerInformation");
                {
                    unsigned char *b = (unsigned char *)info;
                    char hex[3 * 32 + 1];
                    int k;
                    for (k = 0; k < 32; k++) snprintf(hex + 3 * k, 4, "%02x ", b[k]);
                    hex[3 * 32] = 0;
                    log_ts("  real controllerInfo: %s", hex);
                }

                device_id = 0;
                CHECK(scePadGetDeviceId(pad_handle, &device_id), "real GetDeviceId");
                log_ts("  real deviceId: %#llx", (unsigned long long)device_id);
            }
        }
        if (pad_handle < 0) log_ts("  no real pad found");
    }

    /* try the typo version: scePadVertualDeviceAddDevice */
    log_ts("-- scePadVertualDeviceAddDevice (the typo legacy):");
    {
        typedef int32_t (*vertual_fn)(void *, int32_t);
        vertual_fn fn = (vertual_fn)dlsym(RTLD_DEFAULT, "scePadVertualDeviceAddDevice");
        if (fn) {
            int32_t param[8];
            int i;
            for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
            param[0] = (int32_t)sizeof param;
            param[1] = 1;
            r = fn(param, 3);
            log_ts("  VertualAddDevice(param, 3): %#x", (unsigned)r);
            log_ts("  param out: %08x %08x %08x %08x %08x %08x",
                   (unsigned)param[2], (unsigned)param[3], (unsigned)param[4],
                   (unsigned)param[5], (unsigned)param[6], (unsigned)param[7]);
            if (r == 0 && param[2] != (int32_t)0xDEADBEEF) {
                int32_t cap = -1;
                scePadGetCapability(param[2], &cap);
                log_ts("  capability: %#x", (unsigned)cap);
                scePadVirtualDeviceDeleteDevice(param[2]);
            }
        } else {
            log_ts("  not found via dlsym");
        }
    }

    /* try scePadRemoteControlInsertData (a different input path?) */
    log_ts("-- scePadRemoteControlInsertData:");
    {
        typedef int32_t (*rc_fn)(int32_t, const void *, int32_t);
        rc_fn fn = (rc_fn)dlsym(RTLD_DEFAULT, "scePadRemoteControlInsertData");
        if (fn) {
            unsigned char data[120] = { 0 };
            /* just try with zeros to see if it takes a handle */
            log_ts("  found at %p (not called: need a valid handle first)", fn);
        } else {
            log_ts("  not found");
        }
    }

    /* try scePadOutputReport on the virtual (if we have one) */
    /* (already covered above if AddDevice succeeded) */

    /* check scePadGetCapability on the first real handle for reference */
    log_ts("-- scePadGetCapability on all handles:");
    {
        int h;
        for (h = 0; h < 8; h++) {
            int32_t cap = -1;
            r = scePadGetCapability(h, &cap);
            if (r == 0) log_ts("  handle %d: capability %#x", h, (unsigned)cap);
        }
    }

    log_ts("mbusprobe: done");
    if (g_out) fclose(g_out);
    return 0;
}
