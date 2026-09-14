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

static bool g_injected_rom_listed = false;

void boot_log(const char *msg) { printf("[NJEMU] %s\n", msg); }
void dbg_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* Intelligent root scanner: matches requested files against actual disc entries ignoring case & version suffixes */
static void resolve_iso_path(const char *input_path, char *output_path, size_t max_len) {
    if (!input_path || input_path[0] == '\0') {
        strncpy(output_path, "cdrom0:\\", max_len);
        return;
    }

    if (strncasecmp(input_path, "cdrom0:", 7) == 0 || strncasecmp(input_path, "mass0:", 6) == 0 || strncasecmp(input_path, "host:", 5) == 0) {
        strncpy(output_path, input_path, max_len);
        for (int i = 0; output_path[i]; i++) {
            if (output_path[i] == '/') output_path[i] = '\\';
        }
        output_path[max_len - 1] = '\0';
        return;
    }

    const char *filename = strrchr(input_path, '/');
    if (!filename) filename = strrchr(input_path, '\\');
    if (filename) filename++; else filename = input_path;

    if (filename[0] == '.' && (filename[1] == '/' || filename[1] == '\\')) {
        filename += 2;
    }

    if (strcasecmp(filename, ".") == 0 || filename[0] == '\0') {
        strncpy(output_path, "cdrom0:\\", max_len);
        return;
    }

    char matched_name[256];
    bool found = false;

    char search_clean[256];
    strncpy(search_clean, filename, sizeof(search_clean) - 1);
    search_clean[sizeof(search_clean) - 1] = '\0';
    char *semi_req = strchr(search_clean, ';');
    if (semi_req) *semi_req = '\0';

    DIR *dir = __real_opendir("cdrom0:\\");
    if (dir) {
        struct dirent *entry;
        while ((entry = __real_readdir(dir)) != NULL) {
            char entry_clean[256];
            strncpy(entry_clean, entry->d_name, sizeof(entry_clean) - 1);
            entry_clean[sizeof(entry_clean) - 1] = '\0';
            char *semi_ent = strchr(entry_clean, ';');
            if (semi_ent) *semi_ent = '\0';

            if (strcasecmp(entry_clean, search_clean) == 0) {
                strncpy(matched_name, entry->d_name, sizeof(matched_name));
                found = true;
                break;
            }
        }
        closedir(dir);
    }

    if (!found) {
        int i = 0;
        for (; filename[i] && i < sizeof(matched_name) - 1; i++) {
            matched_name[i] = toupper((unsigned char)filename[i]);
        }
        matched_name[i] = '\0';
    }

    snprintf(output_path, max_len, "cdrom0:\\%s", matched_name);
    output_path[max_len - 1] = '\0';
}

/* Linker Wrappers */
FILE *__wrap_fopen(const char *filename, const char *mode) {
    char resolved[1024];
    resolve_iso_path(filename, resolved, sizeof(resolved));
    return __real_fopen(resolved, mode);
}

DIR *__wrap_opendir(const char *name) {
    g_injected_rom_listed = false;
    return __real_opendir("cdrom0:\\");
}

static struct dirent fake_entry;

struct dirent *__wrap_readdir(DIR *dirp) {
    struct dirent *entry = __real_readdir(dirp);
    
    if (!entry && !g_injected_rom_listed) {
        g_injected_rom_listed = true;
        memset(&fake_entry, 0, sizeof(fake_entry));
        strcpy(fake_entry.d_name, "AVSP.ZIP");
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

    chdir("cdrom0:\\");
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