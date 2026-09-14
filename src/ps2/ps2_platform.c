#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include "emumain.h"

#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>
#include <ps2_usb_driver.h>
#include <ps2_mx4sio_driver.h>
#include <ps2_audio_driver.h>
#include <ps2_cdfs_driver.h>

/* HELPER: Converts any path or filename string to lowercase for case-insensitive handling */
static void path_to_lowercase(char *str) {
    if (!str) return;
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

/* BOOT LOG (diagnostic). Writes to CWD (USB root) first, then device paths. */
void boot_log(const char *msg)
{
    /* DEBUG LOG DISABLED (v16 cleanup): no njemu_boot.txt output. */
    (void)msg;
}

typedef struct ps2_platform {
} ps2_platform_t;

static void reset_IOP()
{
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {}
    while (!SifIopSync()) {}
}

static void prepare_IOP()
{
    reset_IOP();
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();
}

static void init_drivers()
{
    init_only_boot_ps2_filesystem_driver();
    init_usb_driver(true);
    init_mx4sio_driver(true);
    init_cdfs_driver();       // Enables cdrom0:/ for ISO / disc loading
    init_audio_driver();
}

static void deinit_drivers()
{
    deinit_audio_driver();
    deinit_only_boot_ps2_filesystem_driver();
}

static void *ps2_init(void) {
    ps2_platform_t *ps2 = (ps2_platform_t*)calloc(1, sizeof(ps2_platform_t));

    prepare_IOP();
    boot_log("[S0] after prepare_IOP");
    init_drivers();
    boot_log("[S1] after init_drivers");

    /* FIX: Robustly check both lowercase and uppercase variants for ISO/disc compatibility */
    if (chdir("roms") != 0 && chdir("ROMS") != 0) {
        if (chdir("cdrom0:/roms") != 0 && chdir("cdrom0:/ROMS") != 0) {
            if (chdir("mass:/roms") != 0 && chdir("mass:/ROMS") != 0) {
                chdir("cdrom0:/"); // Fallback to root if folder search fails
            }
        }
    }

    boot_log("[S2] ps2_init end");
    return ps2;
}

static void ps2_free(void *data) {
    ps2_platform_t *ps2 = (ps2_platform_t*)data;

    deinit_drivers();

    free(ps2);
}

void dbg_printf(const char *fmt, ...)
{
    (void)fmt;
}

static void ps2_main(void *data, int argc, char *argv[]) {
    ps2_platform_t *ps2 = (ps2_platform_t*)data;

    /* Force all command-line arguments (e.g., ROM paths) to lowercase */
    for (int i = 0; i < argc; i++) {
        path_to_lowercase(argv[i]);
    }

    getcwd(screenshotDir, sizeof(screenshotDir));
    strcat(screenshotDir, "/PICTURE");
    mkdir(screenshotDir, 0777);
#if   (EMU_SYSTEM == CPS1)
    strcat(screenshotDir, "/CPS1");
#endif
#if   (EMU_SYSTEM == CPS2)
    strcat(screenshotDir, "/CPS2");
#endif
#if   (EMU_SYSTEM == MVS)
    strcat(screenshotDir, "/MVS");
#endif
#if   (EMU_SYSTEM == NCDZ)
    strcat(screenshotDir, "/NCDZ");
#endif
}

static bool ps2_startSystemButtons(void *data) {
    return false;
}

static int32_t ps2_getDevkitVersion(void *data) {
    return 0;
}

platform_driver_t platform_ps2 = {
    "ps2",
    ps2_init,
    ps2_free,
    ps2_main,
    ps2_startSystemButtons,
    ps2_getDevkitVersion,
};