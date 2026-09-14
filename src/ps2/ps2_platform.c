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

void boot_log(const char *msg) { printf("[NJEMU] %s\n", msg); }
void dbg_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static void force_absolute_path(const char *input, char *output, size_t max_len) {
    if (!input) {
        strcpy(output, g_active_rom_path);
        return;
    }

    if (strncasecmp(input, "cdrom0:", 7) == 0 || strncasecmp(input, "mass0:", 6) == 0 || strncasecmp(input, "host:", 5) == 0) {
        strncpy(output, input, max_len);
        output[max_len - 1] = '\0';
        for (int i = 0; output[i]; i++) {
            if (output[i] == '/') output[i] = '\\';
        }
        return;
    }

    if (strcmp(input, ".") == 0 || strcmp(input, "") == 0 || input[0] != '/') {
        if (input[0] == '.' && (input[1] == '/' || input[1] == '\\')) input += 2;
        
        if (strlen(input) > 0) {
            snprintf(output, max_len, "%s\\%s", g_active_rom_path, input);
        } else {
            strncpy(output, g_active_rom_path, max_len);
        }
    } else {
        snprintf(output, max_len, "%s%s", g_active_rom_path, input);
    }

    output[max_len - 1] = '\0';
    for (int i = 0; output[i]; i++) {
        if (output[i] == '/') output[i] = '\\';
    }
}

/* Linker Wrappers */
FILE *__wrap_fopen(const char *filename, const char *mode) {
    char resolved[1024];
    force_absolute_path(filename, resolved, sizeof(resolved));
    return __real_fopen(resolved, mode);
}

DIR *__wrap_opendir(const char *name) {
    char resolved[1024];
    force_absolute_path(name, resolved, sizeof(resolved));
    DIR *d = __real_opendir(resolved);
    if (!d && strcmp(resolved, g_active_rom_path) != 0) {
        d = __real_opendir(g_active_rom_path);
    }
    return d;
}

struct dirent *__wrap_readdir(DIR *dirp) {
    struct dirent *entry = __real_readdir(dirp);
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
    force_absolute_path(path, resolved, sizeof(resolved));
    return __real_stat(resolved, buf);
}

int __wrap_access(const char *path, int amode) {
    char resolved[1024];
    force_absolute_path(path, resolved, sizeof(resolved));
    return __real_access(resolved, amode);
}

typedef struct ps2_platform {} ps2_platform_t;

static void *ps2_init(void) {
    ps2_platform_t *ps2 = (ps2_platform_t*)calloc(1, sizeof(ps2_platform_t));

    // DO NOT call SifIopReset(NULL, 0) here! 
    // It destroys the BIOS CDVDMAN/CDFS drivers and freezes on black screen.
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    sbv_patch_fileio();

    init_ps2_filesystem_driver();
    init_usb_driver(true);
    init_mx4sio_driver(true);
    init_cdfs_driver();
    init_audio_driver();

    const char *test_paths[] = {
        "cdrom0:\\ROMS", "cdrom0:\\roms", "cdrom0:\\",
        "mass0:\\ROMS", "mass0:\\roms", "mass0:\\"
    };

    for (size_t i = 0; i < sizeof(test_paths)/sizeof(test_paths[0]); i++) {
        DIR *d = __real_opendir(test_paths[i]);
        if (d) {
            strcpy(g_active_rom_path, test_paths[i]);
            closedir(d);
            break;
        }
    }

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