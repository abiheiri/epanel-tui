#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Compare two version strings of the form optional "v" followed by
   dot-separated integers. Returns <0 if a<b, 0 if equal, >0 if a>b.
   Missing trailing components are treated as 0 (so "1.0" == "1.0.0"). */
static int version_compare(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (;;) {
        long av = 0, bv = 0;
        while (*a >= '0' && *a <= '9') { av = av * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { bv = bv * 10 + (*b - '0'); b++; }
        if (av != bv) return (av < bv) ? -1 : 1;
        if (*a != '.' && *b != '.') return 0;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
}

static char *shell_read(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char *out = NULL;
    size_t cap = 0, len = 0;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = cap ? cap * 2 : 65536;
            out = realloc(out, cap);
        }
        memcpy(out + len, buf, n);
        len += n;
    }
    pclose(fp);
    if (out) out[len] = '\0';
    return out;
}

int self_update(const char *current_version, const char *target_triple) {
    const char *api_url = "https://api.github.com/repos/abiheiri/epanel-tui/releases/latest";

    printf("Checking for latest release...\n");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s -L -H \"User-Agent: epanel/%s\" \"%s\"",
        current_version, api_url);
    char *body = shell_read(cmd);
    if (!body || strlen(body) == 0) {
        printf("No updates available.\n");
        free(body);
        return 0;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "Failed to parse release info\n");
        return -1;
    }

    cJSON *tag_name = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (cJSON_IsString(tag_name) &&
        version_compare(tag_name->valuestring, current_version) <= 0) {
        printf("Already up to date (%s).\n", current_version);
        cJSON_Delete(root);
        return 0;
    }

    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) {
        cJSON_Delete(root);
        fprintf(stderr, "No assets found\n");
        return -1;
    }

    char expected_name[256];
    snprintf(expected_name, sizeof(expected_name), "epanel-%s.tar.gz", target_triple);

    char *download_url = NULL;
    size_t n = cJSON_GetArraySize(assets);
    for (size_t i = 0; i < n; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url) && strcmp(name->valuestring, expected_name) == 0) {
            download_url = strdup(url->valuestring);
            break;
        }
    }
    cJSON_Delete(root);

    if (!download_url) {
        fprintf(stderr, "Could not find asset %s in latest release\n", expected_name);
        return -1;
    }

    printf("Downloading update from %s ...\n", download_url);
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/epanel-update-%d", (int)getpid());
    if (mkdir(tmp_dir, 0755) != 0 && errno != EEXIST) {
        free(download_url);
        return -1;
    }

    char tar_path[1024];
    snprintf(tar_path, sizeof(tar_path), "%s/update.tar.gz", tmp_dir);
    snprintf(cmd, sizeof(cmd),
        "curl -s -L -H \"User-Agent: epanel/%s\" -o \"%s\" \"%s\"",
        current_version, tar_path, download_url);
    free(download_url);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Download failed\n");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\"", tar_path, tmp_dir);
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Extract failed\n");
        return -1;
    }

    char new_exe[1024];
    snprintf(new_exe, sizeof(new_exe), "%s/epanel", tmp_dir);
    struct stat st;
    if (stat(new_exe, &st) != 0) {
        fprintf(stderr, "Update archive did not contain expected binary\n");
        return -1;
    }

    char current_exe[1024];
    ssize_t len = readlink("/proc/self/exe", current_exe, sizeof(current_exe) - 1);
    if (len < 0) {
        /* Fallback for macOS */
#ifdef __APPLE__
        uint32_t size = sizeof(current_exe);
        if (_NSGetExecutablePath(current_exe, &size) != 0) {
            fprintf(stderr, "Cannot determine current executable path\n");
            return -1;
        }
#else
        fprintf(stderr, "Cannot determine current executable path\n");
        return -1;
#endif
    } else {
        current_exe[len] = '\0';
    }

    char backup[1024 + 8];
    snprintf(backup, sizeof(backup), "%s.old", current_exe);
    unlink(backup);
    if (rename(current_exe, backup) != 0) {
        fprintf(stderr, "Failed to backup current binary\n");
        return -1;
    }
    if (link(new_exe, current_exe) != 0) {
        if (rename(backup, current_exe) != 0) {
            fprintf(stderr, "Failed to restore backup\n");
        }
        return -1;
    }
    chmod(current_exe, 0755);
    unlink(backup);

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dir);
    int cleanup_rc = system(cmd);
    (void)cleanup_rc;

    printf("Updated to the latest version successfully.\n");
    return 0;
}
