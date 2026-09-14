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

static char g_active_rom_path[256] = "cdrom0:\\ROMS";
static char g_base_device[32] = "cdrom0:\\";

void boot_log(const char *msg) { printf("[NJEMU] %s\n", msg); }
void dbg_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

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

/* Intelligent path resolver: Walks directories segment-by-segment to match ISO uppercase/version-suffixed names */
static void resolve_iso_path(const char *input_path, char *output_path, size_t max_len) {
    if (!input_path || input_path[0] == '\0') {
        strncpy(output_path, g_active_rom_path, max_len);
        output_path[max_len - 1] = '\0';
        return;
    }

    // If already fully qualified device path, clean up slashes
    if (strncasecmp(input_path, "cdrom0:", 7) == 0 || strncasecmp(input_path, "mass0:", 6) == 0 || strncasecmp(input_path, "host:", 5) == 0) {
        strncpy(output_path, input_path, max_len);
        output_path[max_len - 1] = '\0';
        for (int i = 0; output_path[i]; i++) {
            if (output_path[i] == '/') output_path[i] = '\\';
        }
        return;
    }

    char work_path[1024];
    if (strcmp(input_path, ".") == 0) {
        strncpy(output_path, g_active_rom_path, max_len);
        return;
    }

    // Handle relative paths or root-relative paths like "cache/avsp.cache" or "avsp.zip"
    if (input_path[0] == '.' && (input_path[1] == '/' || input_path[1] == '\\')) {
        input_path += 2;
    }

    if (strncasecmp(input_path, "cache", 5) == 0) {
        snprintf(work_path, sizeof(work_path), "%scache%s", g_base_device, input_path + 5);
    } else if (strncasecmp(input_path, "roms", 4) == 0 || strncasecmp(input_path, "ROMS", 4) == 0) {
        // If it explicitly references ROMs, map to active path + remainder
        const char *remainder = input_path + 4;
        if (*remainder == '/' || *remainder == '\\') remainder++;
        if (*remainder != '\0') {
            snprintf(work_path, sizeof(work_path), "%s\\%s", g_active_rom_path, remainder);
        } else {
            strncpy(work_path, g_active_rom_path, sizeof(work_path));
        }
    } else {
        // Treat as a file/folder inside the active ROM path
        snprintf(work_path, sizeof(work_path), "%s\\%s", g_active_rom_path, input_path);
    }

    // Normalize slashes
    for (int i = 0; work_path[i]; i++) {
        if (work_path[i] == '/') work_path[i] = '\\';
    }

    // If targeting cdrom0:, resolve each segment against the ISO filesystem for correct case/version
    if (strncasecmp(work_path, "cdrom0:", 7) == 0) {
        int prefix_len = (strncasecmp(work_path, "cdrom0:\\", 8) == 0) ? 8 : 7;
        char path_tokens[1024];
        strncpy(path_tokens, work_path + prefix_len, sizeof(path_tokens) - 1);
        path_tokens[sizeof(path_tokens) - 1] = '\0';

        char current_dir[1024];
        snprintf(current_dir, sizeof(current_dir), "%.*s", prefix_len, work_path);

        char *token = strtok(path_tokens, "\\");
        while (token != NULL) {
            DIR *dir = __real_opendir(current_dir);
            if (!dir) break;

            bool found = false;
            struct dirent *entry;
            char matched_name[256];

            while ((entry = __real_readdir(dir)) != NULL) {
                if (iso_name_match(entry->d_name, token)) {
                    strncpy(matched_name, entry->d_name, sizeof(matched_name));
                    found = true;
                    break;
                }
            }
            closedir(dir);

            int len = strlen(current_dir);
            if (len > 0 && current_dir[len - 1] != '\\') {
                strcat(current_dir, "\\");
            }
            if (found) {
                strcat(current_dir, matched_name);
            } else {
                // Fallback to uppercase if not found in directory listing
                for (int i = 0; token[i]; i++) token[i] = toupper((unsigned char)token[i]);
                strcat(current_dir, token);
            }

            token = strtok(NULL, "\\");
        }
        strncpy(output_path, current_dir, max_len);
    } else {
        strncpy(output_path, work_path, max_len);
    }
    output[max_len - 1] = '\0';
}

/* Linker Wrappers */
FILE *__wrap_fopen(const char *filename, const char *mode) {
    char resolved[1024];
    resolve_iso_path(filename, resolved, sizeof(resolved));
    return __real_fopen(resolved, mode);
}

DIR *__wrap_opendir(const char *name) {
    char resolved[1024];
    resolve_iso_path(name, resolved, sizeof(resolved));
    DIR *d = __real_opendir(resolved);
    if (!d) {
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

    const char *test_paths[] = {
        "cdrom0:\\ROMS", "cdrom0:\\roms", "cdrom0:\\",
        "mass0:\\ROMS", "mass0:\\roms", "mass0:\\"
    };

    for (size_t i = 0; i < sizeof(test_paths)/sizeof(test_paths[0]); i++) {
        DIR *d = __real_opendir(test_paths[i]);
        if (d) {
            strcpy(g_active_rom_path, test_paths[i]);
            strncpy(g_base_device, test_paths[i], sizeof(g_base_device));
            char *slash = strchr(g_base_device + 7, '\\');
            if (slash) *(slash + 1) = '\0';
            
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