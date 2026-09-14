#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
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

extern FILE *__real_fopen(const char *filename, const char *mode);
extern DIR *__real_opendir(const char *name);
extern struct dirent *__real_readdir(DIR *dirp);
extern int __real_stat(const char *path, struct stat *buf);
extern int __real_access(const char *path, int amode);

static char g_active_rom_path[256] = "cdrom0:\\";
static char g_base_device[32] = "cdrom0:\\";
static bool g_injected_rom_listed = false;

void boot_log(const char *msg) { printf("[NJEMU] %s\n", msg); }
void dbg_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static void resolve_iso_path(const char *input_path, char *output_path, size_t max_len) {
    if (!input_path || input_path[0] == '\0') {
        strncpy(output_path, g_active_rom_path, max_len);
        output_path[max_len - 1] = '\0';
        return;
    }

    if (strncasecmp(input_path, "cdrom0:", 7) == 0 || strncasecmp(input_path, "mass0:", 6) == 0 || strncasecmp(input_path, "host:", 5) == 0) {
        strncpy(output_path, input_path, max_len);
        output_path[max_len - 1] = '\0';
        for (int i = 0; output_path[i]; i++) {
            if (output_path[i] == '/') output_path[i] = '\\';
        }
        return;
    }

    char work_path[1024];
    if (strncasecmp(input_path, "cache", 5) == 0) {
        snprintf(work_path, sizeof(work_path), "%scache%s", g_base_device, input_path + 5);
    } else {
        // Force any filename request (like avsp.zip) to look directly in root/active path
        const char *filename = strrchr(input_path, '/');
        if (!filename) filename = strrchr(input_path, '\\');
        if (filename) filename++; else filename = input_path;

        snprintf(work_path, sizeof(work_path), "%s\\%s", g_active_rom_path, filename);
    }

    for (int i = 0; work_path[i]; i++) {
        if (work_path[i] == '/') work_path[i] = '\\';
    }

    strncpy(output_path, work_path, max_len);
    output_path[max_len - 1] = '\0';
}

/* Linker Wrappers with Forced Injection */
FILE *__wrap_fopen(const char *filename, const char *mode) {
    char resolved[1024];
    resolve_iso_path(filename, resolved, sizeof(resolved));
    FILE *f = __real_fopen(resolved, mode);
    printf("[FOPEN] '%s' -> %s\n", resolved, f ? "SUCCESS" : "FAILED");
    return f;
}

DIR *__wrap_opendir(const char *name) {
    char resolved[1024];
    resolve_iso_path(name, resolved, sizeof(resolved));
    DIR *d = __real_opendir(resolved);
    g_injected_rom_listed = false; // Reset injection tracker on new directory open
    printf("[OPENDIR] '%s' -> %s\n", resolved, d ? "SUCCESS" : "FAILED");
    if (!d) {
        d = __real_opendir(g_active_rom_path);
    }
    return d;
}

static struct dirent fake_entry;

struct dirent *__wrap_readdir(DIR *dirp) {
    struct dirent *entry = __real_readdir(dirp);
    
    // If the real directory scan finishes or finds nothing, inject AVSP.ZIP so it always appears
    if (!entry && !g_injected_rom_listed) {
        g_injected_rom_listed = true;
        memset(&fake_entry, 0, sizeof(fake_entry));
        strcpy(fake_entry.d_name, "avsp.zip");
        printf("[READDIR] Force-injecting ROM: 'avsp.zip'\n");
        return &fake_entry;
    }

    if (entry) {
        char *semi = strchr(entry->d_name, ';');
        if (semi) *semi = '\0';
        for (int i = 0; entry->d_name[i]; i++) {
            entry->d_name[i] = tolower((unsigned char)entry->d_name[i]);
        }
    }
    return entry;
}

int __wrap_stat(const char *path, struct stat *buf) {
    char resolved[1024];
    resolve_iso_path(path, resolved, sizeof(resolved));
    return __real_stat(resolved, buf);
}

int __wrap_access(const char *path, int amode) {
    char resolved[1024];
    resolve_iso_path(path, resolved, sizeof(resolved));
    return __real_access(resolved, amode);
}

typedef struct ps2_platform {} ps2_platform_t;

static void *ps2_init(void) {
    ps2_platform_t *ps2 = (ps2_platform_t*)calloc(1, sizeof(ps2_platform_t));

    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    init_ps2_filesystem_driver();
    init_usb_driver(true);
    init_mx4sio_driver(true);
    init_cdfs_driver();
    init_audio_driver();

    strcpy(g_active_rom_path, "cdrom0:\\");
    strcpy(g_base_device, "cdrom0:\\");

    chdir(g_active_rom_path);
    return ps2;
}

static void ps2_free(void *data) {
    deinit_audio_driver();
    deinit_ps2_filesystem_driver();
    free(data);
}

static void ps2_main(void *data, int argc, char *argv[]) {
    (void)data; (void)argc; (void)argv;
    getcwd(screenshotDir, sizeof(screenshotDir));
    strcat(screenshotDir, "/PICTURE");
    mkdir(screenshotDir, 0777);
}

static bool ps2_startSystemButtons(void *data) { return false; }
static int32_t ps2_getDevkitVersion(void *data) { return 0; }

platform_driver_t platform_ps2 = {
    "ps2",
    ps2_init,
    ps2_free,
    ps2_main,
    ps2_startSystemButtons,
    ps2_getDevkitVersion,
};