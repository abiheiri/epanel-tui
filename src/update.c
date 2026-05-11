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

/* Download a URL to a file and return the HTTP status code, or -1 on error. */
static long download_file(const char *url, const char *out_path,
                          const char *current_version) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s -L -H \"User-Agent: epanel/%s\" -o \"%s\" -w \"%%{http_code}\" \"%s\"",
        current_version, out_path, url);
    char *status_str = shell_read(cmd);
    if (!status_str) return -1;
    long status = atol(status_str);
    free(status_str);
    return status;
}

/* Extract the version tag from GitHub's /releases/latest redirect headers.
   Returns 0 on success with *out_version set, -1 on failure. */
static int fetch_version_web(char **out_version) {
    const char *cmd =
        "curl -s -I -L -H \"User-Agent: epanel\" "
        "\"https://github.com/abiheiri/epanel-tui/releases/latest\"";
    char *headers = shell_read(cmd);
    if (!headers || strlen(headers) == 0) {
        free(headers);
        return -1;
    }

    char *tag = strstr(headers, "/tag/");
    if (!tag) {
        free(headers);
        return -1;
    }

    char *start = tag + 5; /* skip "/tag/" */
    char *end = start;
    while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;

    size_t len = end - start;
    if (len == 0 || len > 63) {
        free(headers);
        return -1;
    }

    *out_version = malloc(len + 1);
    if (*out_version) {
        memcpy(*out_version, start, len);
        (*out_version)[len] = '\0';
    }
    free(headers);
    return *out_version ? 0 : -1;
}

/* Shared install logic: extract tarball, verify binary, backup current,
   replace, cleanup. Returns 0 on success, -1 on failure. */
static int install_update(const char *tmp_dir, const char *tar_path) {
    char cmd[2048];

    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\"", tar_path, tmp_dir);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Extract failed.\n");
        return -1;
    }

    char new_exe[1024];
    snprintf(new_exe, sizeof(new_exe), "%s/epanel", tmp_dir);
    struct stat st;
    if (stat(new_exe, &st) != 0) {
        fprintf(stderr, "Update archive did not contain expected binary.\n");
        return -1;
    }

    char current_exe[1024];
    ssize_t len = readlink("/proc/self/exe", current_exe, sizeof(current_exe) - 1);
    if (len < 0) {
#ifdef __APPLE__
        uint32_t size = sizeof(current_exe);
        if (_NSGetExecutablePath(current_exe, &size) != 0) {
            fprintf(stderr, "Cannot determine current executable path.\n");
            return -1;
        }
#else
        fprintf(stderr, "Cannot determine current executable path.\n");
        return -1;
#endif
    } else {
        current_exe[len] = '\0';
    }

    char backup[1024 + 8];
    snprintf(backup, sizeof(backup), "%s.old", current_exe);
    unlink(backup);
    if (rename(current_exe, backup) != 0) {
        fprintf(stderr, "Failed to backup current binary.\n");
        return -1;
    }
    if (link(new_exe, current_exe) != 0) {
        if (rename(backup, current_exe) != 0) {
            fprintf(stderr, "Failed to restore backup.\n");
        }
        return -1;
    }
    chmod(current_exe, 0755);
    unlink(backup);

    printf("Updated to the latest version successfully.\n");
    return 0;
}

/* Try updating via GitHub web redirects (no API).  Returns 0 and sets
   *out_tar_path / *out_tmp_dir on success.  Returns 1 if already up to date,
   2 if no asset exists for this platform, and -1 for errors that should
   trigger the API fallback. */
static int update_via_web(const char *current_version, const char *target_triple,
                          char **out_tar_path, char **out_tmp_dir) {
    printf("Checking for latest release (web)...\n");

    char *latest_version = NULL;
    if (fetch_version_web(&latest_version) != 0) {
        return -1;
    }

    if (version_compare(latest_version, current_version) <= 0) {
        printf("Already up to date (%s).\n", current_version);
        free(latest_version);
        return 1;
    }

    printf("New version available: %s (current: %s)\n",
           latest_version, current_version);
    free(latest_version);

    char *tmp_dir = malloc(256);
    if (!tmp_dir) return -1;
    snprintf(tmp_dir, 256, "/tmp/epanel-update-%d", (int)getpid());
    if (mkdir(tmp_dir, 0755) != 0 && errno != EEXIST) {
        free(tmp_dir);
        return -1;
    }

    char *tar_path = malloc(1024);
    if (!tar_path) {
        free(tmp_dir);
        return -1;
    }
    snprintf(tar_path, 1024, "%s/update.tar.gz", tmp_dir);

    char url[512];
    snprintf(url, sizeof(url),
        "https://github.com/abiheiri/epanel-tui/releases/latest/download/"
        "epanel-%s.tar.gz", target_triple);

    printf("Downloading update...\n");
    long http_status = download_file(url, tar_path, current_version);

    if (http_status == 404) {
        fprintf(stderr,
            "No build available for %s in the latest release.\n", target_triple);
        free(tar_path);
        rmdir(tmp_dir);
        free(tmp_dir);
        return 2;
    }

    if (http_status != 200) {
        fprintf(stderr, "Download failed (HTTP %ld).\n", http_status);
        free(tar_path);
        rmdir(tmp_dir);
        free(tmp_dir);
        return -1;
    }

    struct stat st;
    if (stat(tar_path, &st) != 0 || st.st_size == 0) {
        fprintf(stderr, "Downloaded file is empty or missing.\n");
        free(tar_path);
        rmdir(tmp_dir);
        free(tmp_dir);
        return -1;
    }

    *out_tar_path = tar_path;
    *out_tmp_dir = tmp_dir;
    return 0;
}

/* Fallback: update via GitHub API.  Returns 0 and sets out-paths on success,
   1 if already up to date, -1 on failure. */
static int update_via_api(const char *current_version, const char *target_triple,
                          char **out_tar_path, char **out_tmp_dir) {
    printf("Checking for latest release (API fallback)...\n");

    const char *api_url =
        "https://api.github.com/repos/abiheiri/epanel-tui/releases/latest";
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s -L -H \"User-Agent: epanel/%s\" \"%s\"",
        current_version, api_url);
    char *body = shell_read(cmd);
    if (!body || strlen(body) == 0) {
        fprintf(stderr, "Could not reach GitHub API.\n");
        free(body);
        return -1;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fprintf(stderr, "Failed to parse release info from GitHub API.\n");
        return -1;
    }

    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(message)) {
        fprintf(stderr, "GitHub API error: %s\n", message->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *tag_name = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag_name)) {
        fprintf(stderr, "GitHub API response missing version tag.\n");
        cJSON_Delete(root);
        return -1;
    }

    if (version_compare(tag_name->valuestring, current_version) <= 0) {
        printf("Already up to date (%s).\n", current_version);
        cJSON_Delete(root);
        return 1;
    }

    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) {
        fprintf(stderr, "Latest release has no downloadable assets.\n");
        cJSON_Delete(root);
        return -1;
    }

    char expected_name[256];
    snprintf(expected_name, sizeof(expected_name),
        "epanel-%s.tar.gz", target_triple);

    char *download_url = NULL;
    size_t n = cJSON_GetArraySize(assets);
    for (size_t i = 0; i < n; i++) {
        cJSON *asset = cJSON_GetArrayItem(assets, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *url = cJSON_GetObjectItemCaseSensitive(asset,
            "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url) &&
            strcmp(name->valuestring, expected_name) == 0) {
            download_url = strdup(url->valuestring);
            break;
        }
    }
    cJSON_Delete(root);

    if (!download_url) {
        fprintf(stderr,
            "Could not find asset %s in latest release.\n", expected_name);
        return -1;
    }

    char *tmp_dir = malloc(256);
    if (!tmp_dir) {
        free(download_url);
        return -1;
    }
    snprintf(tmp_dir, 256, "/tmp/epanel-update-%d", (int)getpid());
    if (mkdir(tmp_dir, 0755) != 0 && errno != EEXIST) {
        free(download_url);
        free(tmp_dir);
        return -1;
    }

    char *tar_path = malloc(1024);
    if (!tar_path) {
        free(download_url);
        free(tmp_dir);
        return -1;
    }
    snprintf(tar_path, 1024, "%s/update.tar.gz", tmp_dir);

    printf("Downloading update from %s ...\n", download_url);
    snprintf(cmd, sizeof(cmd),
        "curl -s -L -H \"User-Agent: epanel/%s\" -o \"%s\" \"%s\"",
        current_version, tar_path, download_url);
    free(download_url);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Download failed.\n");
        free(tar_path);
        rmdir(tmp_dir);
        free(tmp_dir);
        return -1;
    }

    *out_tar_path = tar_path;
    *out_tmp_dir = tmp_dir;
    return 0;
}

int self_update(const char *current_version, const char *target_triple) {
    char *tar_path = NULL;
    char *tmp_dir = NULL;
    int rc;

    rc = update_via_web(current_version, target_triple, &tar_path, &tmp_dir);
    if (rc == 1) return 0;          /* already up to date */
    if (rc == 2) return -1;         /* no asset for platform */
    if (rc != 0) {
        printf("Trying fallback update method...\n");
        rc = update_via_api(current_version, target_triple, &tar_path, &tmp_dir);
        if (rc == 1) return 0;      /* already up to date */
        if (rc != 0) goto cleanup;
    }

    rc = install_update(tmp_dir, tar_path);

cleanup:
    if (tmp_dir) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", tmp_dir);
        int cleanup_rc = system(cmd);
        (void)cleanup_rc;
        free(tar_path);
        free(tmp_dir);
    }
    return rc;
}
