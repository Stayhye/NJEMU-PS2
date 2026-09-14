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

static char g_iso_rom_base[256] = "";

/* ISO9660 Case-Insensitive & Version-Suffix (e.g. ;1) File Matching */
static bool iso_name_match(const char *filename, const char *search_name) {
    char f_copy[256];
    char s_copy[256];
    strncpy(f_copy, filename, sizeof(f_copy) - 1);
    f_copy[sizeof(f_copy) - 1] = '\0';
    strncpy(s_copy, search_name, sizeof(s_copy) - 1);
    s_copy[sizeof(s_copy) - 1] = '\0';

    char *semi = strchr(f_copy, ';');
    if (semi) *semi = '\0';
    char *semi2 = strchr(s_copy, ';');
    if (semi2) *semi2 = '\0';

    return strcasecmp(f_copy, s_copy) == 0;
}

static void resolve_case_path(const char *input_path, char *output_path, size_t max_len) {
    if (!input_path) {
        output_path[0] = '\0';
        return;
    }

    char temp_path[1024];
    if ((strcmp(input_path, ".") == 0 || strcmp(input_path, "") == 0) && g_iso_rom_base[0] != '\0') {
        strncpy(output_path, g_iso_rom_base, max_len);
        output_path[max_len - 1] = '\0';
        return;
    }

    if (input_path[0] != '/' && strchr(input_path, ':') == NULL) {
        if (g_iso_rom_base[0] != '\0') {
            snprintf(temp_path, sizeof(temp_path), "%s/%s", g_iso_rom_base, input_path);
            input_path = temp_path;
        }
    }

    strncpy(output_path, input_path, max_len);
    output_path[max_len - 1] = '\0';

    if (strncasecmp(output_path, "cdrom0:/", 8) == 0 || strncasecmp(output_path, "cdrom:/", 7) == 0) {
        int prefix_len = (strncasecmp(output_path, "cdrom0:/", 8) == 0) ? 8 : 7;
        char work_path[1024];
        strncpy(work_path, output_path + prefix_len, sizeof(work_path) - 1);
        work_path[sizeof(work_path) - 1] = '\0';

        char current_dir[1024];
        snprintf(current_dir, sizeof(current_dir), "%.*s", prefix_len, output_path);

        char *token = strtok(work_path, "/\\");
        while (token != NULL) {
            if (strcmp(token, ".") == 0) {
                token = strtok(NULL, "/\\");
                continue;
            }

            DIR *dir = opendir(current_dir);
            if (!dir) break;

            bool found = false;
            struct dirent *entry;
            char matched_name[256];

            while ((entry = readdir(dir)) != NULL) {
                if (iso_name_match(entry->d_name, token)) {
                    strncpy(matched_name, entry->d_name, sizeof(matched_name));
                    found = true;
                    break;
                }
            }
            closedir(dir);

            if (current_dir[strlen(current_dir) - 1] != '/') {
                strcat(current_dir, "/");
            }
            if (found) {
                strcat(current_dir, matched_name);
            } else {
                strcat(current_dir, token);
            }

            token = strtok(NULL, "/\\");
        }
        strncpy(output_path, current_dir, max_len);
        output_path[max_len - 1] = '\0';
    }
}

/* Linker Wrappers: Intercept filesystem calls for total case-insensitivity */
extern FILE *__real_fopen(const char *filename, const char *mode);
extern DIR *__real_opendir(const char *name);
extern struct dirent *__real_readdir(DIR *dirp);
extern int __real_stat(const char *path, struct stat *buf);
extern int __real_access(const char *path, int amode);

FILE *__wrap_fopen(const char *filename, const char *mode) {
    char resolved[1024];
    resolve_case_path(filename, resolved, sizeof(resolved));
    return __real_fopen(resolved, mode);
}

DIR *__wrap_opendir(const char *name) {
    char resolved[1024];
    resolve_case_path(name, resolved, sizeof(resolved));
    return __real_opendir(resolved);
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
    resolve_case_path(path, resolved, sizeof(resolved));
    return __real_stat(resolved, buf);
}

int __wrap_access(const char *path, int amode) {
    char resolved[1024];
    resolve_case_path(path, resolved, sizeof(resolved));
    return __real_access(resolved, amode);
}

/* BOOT LOG */
void boot_log(const char *msg)
{
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
    init_ps2_filesystem_driver();
    init_usb_driver(true);
    init_mx4sio_driver(true);
    init_cdfs_driver();
    init_audio_driver();
}

static void deinit_drivers()
{
    deinit_audio_driver();
    deinit_ps2_filesystem_driver();
}

static void *ps2_init(void) {
    ps2_platform_t *ps2 = (ps2_platform_t*)calloc(1, sizeof(ps2_platform_t));

    prepare_IOP();
    init_drivers();

    /* Automatically detect where ROMs are located on CD, USB, or CWD */
    DIR *d = __real_opendir("cdrom0:/ROMS");
    if (d) { closedir(d); strcpy(g_iso_rom_base, "cdrom0:/ROMS"); }
    else {
        d = __real_opendir("cdrom0:/roms");
        if (d) { closedir(d); strcpy(g_iso_rom_base, "cdrom0:/roms"); }
        else {
            d = __real_opendir("ROMS");
            if (d) { closedir(d); strcpy(g_iso_rom_base, "ROMS"); }
            else {
                d = __real_opendir("roms");
                if (d) { closedir(d); strcpy(g_iso_rom_base, "roms"); }
            }
        }
    }

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

    for (int i = 0; i < argc; i++) {
        char resolved[1024];
        resolve_case_path(argv[i], resolved, sizeof(resolved));
        strncpy(argv[i], resolved, strlen(argv[i]) + 1);
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