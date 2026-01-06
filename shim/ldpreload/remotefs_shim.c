// RemoteFS LD_PRELOAD shim. Intercepts POSIX file operations under REMOTEFS_ROOT
// and proxies them to the remotefs-daemon over a Unix socket.
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/stat.h>
#endif

#if defined(__linux__) && defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 28)
#define REMOTEFS_HAVE_STATX 1
#endif
#endif

#ifndef REMOTEFS_HAVE_STATX
#define REMOTEFS_HAVE_STATX 0
#endif

#if REMOTEFS_HAVE_STATX && !defined(STATX_BASIC_STATS)
#define STATX_BASIC_STATS 0x000007ffU
#endif

#include <curl/curl.h>

#define JSMN_PARENT_LINKS
#include "jsmn.h"

#ifndef O_PATH
#define O_PATH 0
#endif

// shim_config captures runtime configuration resolved from environment
// variables. Paths are stored in canonical form to simplify comparisons.
struct shim_config {
    char root[PATH_MAX];
    size_t root_len;
    char socket_path[PATH_MAX];
    char cache_dir[PATH_MAX];
};

// buffer is used by curl callbacks to accumulate JSON payloads.
struct buffer {
    char *data;
    size_t len;
};

// remote_meta mirrors the metadata schema returned by the daemon's /stat API.
struct remote_meta {
    char path[PATH_MAX];
    long long size;
    long long mode;
    int uid;
    int gid;
    int is_dir;
};

// dir_entry_pair keeps dirent32/dirent64 siblings so both readdir variants can
// return cached listings.
struct dir_entry_pair {
    struct dirent *d32;
#ifdef __USE_LARGEFILE64
    struct dirent64 *d64;
#endif
};

// shim_dir tracks a directory listing that has been materialized from /ls so
// future readdir calls can iterate it without contacting the daemon again.
struct shim_dir {
    struct shim_dir *next;
    size_t count;
    size_t index;
    char *abs_path;
    char *rel_path;
    struct dir_entry_pair *entries;
};

typedef int (*open_func)(const char *, int, ...);

static struct shim_config g_cfg;
static uid_t g_uid;
static gid_t g_gid;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_dir_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shim_dir *g_dirs;

static int (*real_stat_fn)(const char *, struct stat *);
static int (*real_lstat_fn)(const char *, struct stat *);
static int (*real_stat64_fn)(const char *, struct stat64 *);
static int (*real_lstat64_fn)(const char *, struct stat64 *);
static int (*real___xstat_fn)(int, const char *, struct stat *);
static int (*real___lxstat_fn)(int, const char *, struct stat *);
static int (*real___xstat64_fn)(int, const char *, struct stat64 *);
static int (*real___lxstat64_fn)(int, const char *, struct stat64 *);
static int (*real___fxstatat_fn)(int, int, const char *, struct stat *, int);
static int (*real_fstatat_fn)(int, const char *, struct stat *, int);
static int (*real_open_fn)(const char *, int, ...);
static int (*real_open64_fn)(const char *, int, ...);
static int (*real_openat_fn)(int, const char *, int, ...);
static DIR *(*real_opendir_fn)(const char *);
static struct dirent *(*real_readdir_fn)(DIR *);
#ifdef __USE_LARGEFILE64
static struct dirent64 *(*real_readdir64_fn)(DIR *);
#endif
static int (*real_readdir_r_fn)(DIR *, struct dirent *, struct dirent **);
#ifdef __USE_LARGEFILE64
static int (*real_readdir64_r_fn)(DIR *, struct dirent64 *, struct dirent64 **);
#endif
static int (*real_closedir_fn)(DIR *);
static void (*real_rewinddir_fn)(DIR *);
static long (*real_telldir_fn)(DIR *);
static void (*real_seekdir_fn)(DIR *, long);
static int (*real_dirfd_fn)(DIR *);
static int (*real_access_fn)(const char *, int);
static int (*real_faccessat_fn)(int, const char *, int, int);
#if REMOTEFS_HAVE_STATX
static int (*real_statx_fn)(int, const char *, int, unsigned int, struct statx *);
#endif

static void init_real_symbols(void);
static bool canonicalize_path(const char *path, char *out, size_t out_len);
static bool resolve_absolute(int dirfd, const char *path, char *out, size_t out_len);
static bool path_within_root(const char *path, char *abs_out, size_t abs_len, char *rel_out, size_t rel_len);
static uint64_t hash_path(const char *path);
static char *url_encode(const char *input);
static int perform_http_request(const char *endpoint, const char *abs_path, curl_write_callback write_cb, void *userdata, long *status);
static int fetch_json(const char *endpoint, const char *abs_path, struct buffer *buf, long *status);
static int parse_json_document(const char *json, jsmntok_t **tokens_out);
static int parse_object_at(const char *json, jsmntok_t *tokens, int idx, struct remote_meta *meta, int *next_idx);
static int hydrate_stat(const char *abs_path, const struct remote_meta *meta, struct stat *st);
#ifdef __USE_LARGEFILE64
static int hydrate_stat64(const char *abs_path, const struct remote_meta *meta, struct stat64 *st);
#endif
static int remote_stat_path(const char *abs_path, struct stat *st);
#ifdef __USE_LARGEFILE64
static int remote_stat64_path(const char *abs_path, struct stat64 *st);
#endif
static int remote_open_file(const char *abs_path, int flags);
static int download_into_temp(const char *abs_path, int fd);
static int populate_directory(const char *abs_path, const char *rel_path, struct shim_dir *dir, const char *json);
static void register_dir(struct shim_dir *dir);
static struct shim_dir *lookup_dir(DIR *handle);
static void unregister_dir(struct shim_dir *dir);
static void free_dir(struct shim_dir *dir);
static struct dirent *dir_next_entry(struct shim_dir *dir);
#ifdef __USE_LARGEFILE64
static struct dirent64 *dir_next_entry64(struct shim_dir *dir);
#endif
static void ensure_initialized(void);

// shim_init_once resolves environment variables, initializes libcurl, and
// captures the process credentials so remote metadata can be rewritten with
// local UID/GID.
static void shim_init_once(void) {
    const char *root = getenv("REMOTEFS_ROOT");
    if (root == NULL || root[0] == '\0') {
        root = "/remote";
    }
    if (!canonicalize_path(root, g_cfg.root, sizeof(g_cfg.root))) {
        strncpy(g_cfg.root, "/remote", sizeof(g_cfg.root) - 1);
        g_cfg.root[sizeof(g_cfg.root) - 1] = '\0';
    }
    g_cfg.root_len = strlen(g_cfg.root);
    if (g_cfg.root_len > 1 && g_cfg.root[g_cfg.root_len - 1] == '/') {
        g_cfg.root[--g_cfg.root_len] = '\0';
    }
    const char *sock = getenv("REMOTEFS_SOCKET");
    if (sock == NULL || sock[0] == '\0') {
        sock = "/tmp/remotefs.sock";
    }
    strncpy(g_cfg.socket_path, sock, sizeof(g_cfg.socket_path) - 1);
    g_cfg.socket_path[sizeof(g_cfg.socket_path) - 1] = '\0';
    const char *cache = getenv("REMOTEFS_SHIM_CACHE");
    if (cache == NULL || cache[0] == '\0') {
        const char *tmp = getenv("TMPDIR");
        if (tmp == NULL || tmp[0] == '\0') {
            tmp = "/tmp";
        }
        snprintf(g_cfg.cache_dir, sizeof(g_cfg.cache_dir), "%s/remotefs-shim", tmp);
    } else {
        strncpy(g_cfg.cache_dir, cache, sizeof(g_cfg.cache_dir) - 1);
        g_cfg.cache_dir[sizeof(g_cfg.cache_dir) - 1] = '\0';
    }
    mkdir(g_cfg.cache_dir, 0700);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    atexit(curl_global_cleanup);
    g_uid = geteuid();
    g_gid = getegid();
    init_real_symbols();
}

static void ensure_initialized(void) {
    pthread_once(&g_init_once, shim_init_once);
}

static void init_real_symbols(void) {
    real_stat_fn = dlsym(RTLD_NEXT, "stat");
    real_lstat_fn = dlsym(RTLD_NEXT, "lstat");
    real_stat64_fn = dlsym(RTLD_NEXT, "stat64");
    real_lstat64_fn = dlsym(RTLD_NEXT, "lstat64");
    real___xstat_fn = dlsym(RTLD_NEXT, "__xstat");
    real___lxstat_fn = dlsym(RTLD_NEXT, "__lxstat");
    real___xstat64_fn = dlsym(RTLD_NEXT, "__xstat64");
    real___lxstat64_fn = dlsym(RTLD_NEXT, "__lxstat64");
    real___fxstatat_fn = dlsym(RTLD_NEXT, "__fxstatat");
    real_fstatat_fn = dlsym(RTLD_NEXT, "fstatat");
    real_open_fn = dlsym(RTLD_NEXT, "open");
    real_open64_fn = dlsym(RTLD_NEXT, "open64");
    real_openat_fn = dlsym(RTLD_NEXT, "openat");
    real_opendir_fn = dlsym(RTLD_NEXT, "opendir");
    real_readdir_fn = dlsym(RTLD_NEXT, "readdir");
#ifdef __USE_LARGEFILE64
    real_readdir64_fn = dlsym(RTLD_NEXT, "readdir64");
    real_readdir64_r_fn = dlsym(RTLD_NEXT, "readdir64_r");
#endif
    real_readdir_r_fn = dlsym(RTLD_NEXT, "readdir_r");
    real_closedir_fn = dlsym(RTLD_NEXT, "closedir");
    real_rewinddir_fn = dlsym(RTLD_NEXT, "rewinddir");
    real_telldir_fn = dlsym(RTLD_NEXT, "telldir");
    real_seekdir_fn = dlsym(RTLD_NEXT, "seekdir");
    real_dirfd_fn = dlsym(RTLD_NEXT, "dirfd");
    real_access_fn = dlsym(RTLD_NEXT, "access");
    real_faccessat_fn = dlsym(RTLD_NEXT, "faccessat");
#if REMOTEFS_HAVE_STATX
    real_statx_fn = dlsym(RTLD_NEXT, "statx");
#endif
}

// canonicalize_path resolves relative elements and collapse "."/".." segments so
// comparisons against the protected root become reliable.
static bool canonicalize_path(const char *path, char *out, size_t out_len) {
    if (path == NULL || out_len == 0) {
        return false;
    }
    char scratch[PATH_MAX];
    if (path[0] == '/') {
        strncpy(scratch, path, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return false;
        }
        snprintf(scratch, sizeof(scratch), "%s/%s", cwd, path);
    }
    char result[PATH_MAX];
    size_t res_len = 0;
    result[res_len++] = '/';
    int stack[PATH_MAX / 2];
    int top = 1;
    stack[0] = 1;
    const char *p = scratch;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *segment_start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - segment_start);
        if (seg_len == 0) {
            continue;
        }
        if (seg_len == 1 && strncmp(segment_start, ".", 1) == 0) {
            continue;
        }
        if (seg_len == 2 && strncmp(segment_start, "..", 2) == 0) {
            if (top > 1) {
                top--;
                res_len = (size_t)stack[top - 1];
                result[res_len] = '\0';
            }
            continue;
        }
        if (res_len > 1) {
            result[res_len++] = '/';
        }
        if (res_len + seg_len >= sizeof(result)) {
            return false;
        }
        memcpy(result + res_len, segment_start, seg_len);
        res_len += seg_len;
        result[res_len] = '\0';
        stack[top++] = (int)res_len;
    }
    if (res_len == 0) {
        result[res_len++] = '/';
    }
    result[res_len] = '\0';
    if (res_len >= out_len) {
        return false;
    }
    strncpy(out, result, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

// dirfd_path obtains the absolute path backing the provided directory file
// descriptor so openat-style calls can be resolved.
static bool dirfd_path(int dirfd, char *out, size_t out_len) {
    if (dirfd == AT_FDCWD || out_len == 0) {
        return false;
    }
#if defined(__linux__)
    char link_path[PATH_MAX];
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", dirfd);
    ssize_t n = readlink(proc_path, link_path, sizeof(link_path) - 1);
    if (n < 0) {
        return false;
    }
    link_path[n] = '\0';
    return canonicalize_path(link_path, out, out_len);
#elif defined(__APPLE__)
    char tmp[PATH_MAX];
    if (fcntl(dirfd, F_GETPATH, tmp) != 0) {
        return false;
    }
    return canonicalize_path(tmp, out, out_len);
#else
    (void)dirfd;
    (void)out;
    (void)out_len;
    return false;
#endif
}

// resolve_absolute combines dirfd+path into an absolute path to feed into
// canonicalize_path.
static bool resolve_absolute(int dirfd, const char *path, char *out, size_t out_len) {
    if (path == NULL) {
        return false;
    }
    if (path[0] == '/') {
        return canonicalize_path(path, out, out_len);
    }
    if (dirfd == AT_FDCWD) {
        return canonicalize_path(path, out, out_len);
    }
    char base[PATH_MAX];
    if (!dirfd_path(dirfd, base, sizeof(base))) {
        return false;
    }
    char combined[PATH_MAX];
    if (snprintf(combined, sizeof(combined), "%s/%s", base, path) >= (int)sizeof(combined)) {
        return false;
    }
    return canonicalize_path(combined, out, out_len);
}

// path_within_root verifies the path stays under REMOTEFS_ROOT and optionally
// returns both absolute and relative forms.
static bool path_within_root(const char *path, char *abs_out, size_t abs_len, char *rel_out, size_t rel_len) {
    char normalized[PATH_MAX];
    if (!canonicalize_path(path, normalized, sizeof(normalized))) {
        return false;
    }
    size_t norm_len = strlen(normalized);
    if (strncmp(normalized, g_cfg.root, g_cfg.root_len) != 0) {
        return false;
    }
    size_t offset = g_cfg.root_len;
    if (g_cfg.root_len > 1) {
        if (offset < norm_len && normalized[offset] == '/') {
            offset++;
        } else if (offset < norm_len) {
            return false;
        }
    } else {
        if (offset < norm_len && normalized[offset] == '/') {
            offset++;
        }
    }
    if (abs_out != NULL && abs_len > 0) {
        strncpy(abs_out, normalized, abs_len - 1);
        abs_out[abs_len - 1] = '\0';
    }
    if (rel_out != NULL && rel_len > 0) {
        const char *rel = normalized + offset;
        if (offset >= norm_len) {
            rel = "";
        }
        strncpy(rel_out, rel, rel_len - 1);
        rel_out[rel_len - 1] = '\0';
    }
    return true;
}

// hash_path generates a stable inode surrogate from the full path. This keeps
// tools such as ls happy even though data is remote.
static uint64_t hash_path(const char *path) {
    uint64_t hash = 1469598103934665603ULL;
    while (path != NULL && *path != '\0') {
        hash ^= (unsigned char)(*path);
        hash *= 1099511628211ULL;
        path++;
    }
    return hash;
}

// url_encode converts arbitrary paths into safe query parameters for the Unix
// domain socket HTTP transport.
static char *url_encode(const char *input) {
    size_t len = strlen(input);
    size_t max = len * 3 + 1;
    char *out = malloc(max);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out[pos++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0xF];
            out[pos++] = hex[c & 0xF];
        }
    }
    out[pos] = '\0';
    return out;
}

// buffer_write appends HTTP responses to a growable buffer.
static size_t buffer_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct buffer *buf = userdata;
    size_t total = size * nmemb;
    char *next = realloc(buf->data, buf->len + total + 1);
    if (next == NULL) {
        return 0;
    }
    buf->data = next;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

struct file_writer {
    int fd;
};

// file_write streams HTTP payloads into a temporary file descriptor.
static size_t file_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct file_writer *writer = userdata;
    size_t total = size * nmemb;
    const char *data = ptr;
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(writer->fd, data + written, total - written);
        if (n <= 0) {
            return 0;
        }
        written += (size_t)n;
    }
    return total;
}

// perform_http_request wraps curl for GET requests directed at the daemon's
// Unix socket, sending ?path=<encoded> query parameters.
static int perform_http_request(const char *endpoint, const char *abs_path, curl_write_callback write_cb, void *userdata, long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        errno = EIO;
        return -1;
    }
    char *encoded = url_encode(abs_path);
    if (!encoded) {
        curl_easy_cleanup(curl);
        errno = ENOMEM;
        return -1;
    }
    char url[PATH_MAX * 2];
    snprintf(url, sizeof(url), "http://unix%s?path=%s", endpoint, encoded);
    free(encoded);
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, g_cfg.socket_path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        errno = EIO;
        return -1;
    }
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    if (status) {
        *status = http_status;
    }
    return 0;
}

// fetch_json materializes JSON responses into memory for /stat and /ls.
static int fetch_json(const char *endpoint, const char *abs_path, struct buffer *buf, long *status) {
    buf->data = NULL;
    buf->len = 0;
    int rc = perform_http_request(endpoint, abs_path, buffer_write, buf, status);
    if (rc != 0) {
        free(buf->data);
        buf->data = NULL;
        buf->len = 0;
        return -1;
    }
    return 0;
}

// parse_json_document runs jsmn with exponential backoff for token capacity so
// the shim can process arbitrarily-sized listings.
static int parse_json_document(const char *json, jsmntok_t **tokens_out) {
    jsmn_parser parser;
    int tok_cap = 32;
    size_t len = strlen(json);
    if (tokens_out) {
        *tokens_out = NULL;
    }
    for (;;) {
        jsmn_init(&parser);
        jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * tok_cap);
        if (!tokens) {
            return -1;
        }
        int ret = jsmn_parse(&parser, json, len, tokens, tok_cap);
        if (ret >= 0) {
            *tokens_out = tokens;
            return ret;
        }
        free(tokens);
        if (ret == JSMN_ERROR_NOMEM) {
            tok_cap *= 2;
            if (tok_cap > 4096) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

static bool token_equals(const char *json, jsmntok_t *tok, const char *s) {
    size_t len = (size_t)(tok->end - tok->start);
    size_t target_len = strlen(s);
    return len == target_len && strncmp(json + tok->start, s, len) == 0;
}

static long long parse_long(const char *json, jsmntok_t *tok) {
    char tmp[64];
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, json + tok->start, len);
    tmp[len] = '\0';
    return strtoll(tmp, NULL, 10);
}

// parse_object_at walks a single POSIXEntry object and copies interesting
// fields into remote_meta.
static int parse_object_at(const char *json, jsmntok_t *tokens, int idx, struct remote_meta *meta, int *next_idx) {
    if (tokens[idx].type != JSMN_OBJECT) {
        return -1;
    }
    int fields = tokens[idx].size;
    idx++;
    struct remote_meta tmp = {0};
    tmp.uid = (int)g_uid;
    tmp.gid = (int)g_gid;
    for (int i = 0; i < fields; i++) {
        jsmntok_t *key = &tokens[idx++];
        jsmntok_t *val = &tokens[idx++];
        if (key->type != JSMN_STRING) {
            return -1;
        }
        if (token_equals(json, key, "Path")) {
            size_t len = (size_t)(val->end - val->start);
            if (len >= sizeof(tmp.path)) {
                len = sizeof(tmp.path) - 1;
            }
            memcpy(tmp.path, json + val->start, len);
            tmp.path[len] = '\0';
        } else if (token_equals(json, key, "Size")) {
            tmp.size = parse_long(json, val);
        } else if (token_equals(json, key, "Mode")) {
            tmp.mode = parse_long(json, val);
        } else if (token_equals(json, key, "UID")) {
            tmp.uid = (int)parse_long(json, val);
        } else if (token_equals(json, key, "GID")) {
            tmp.gid = (int)parse_long(json, val);
        } else if (token_equals(json, key, "IsDir")) {
            tmp.is_dir = (json[val->start] == 't' || json[val->start] == 'T');
        }
    }
    *meta = tmp;
    if (next_idx) {
        *next_idx = idx;
    }
    return 0;
}

// fill_time centralizes how we stamp synthetic timestamps when the daemon
// omits them.
static void fill_time(struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
}

// derive_mode hydrates type/perms when the remote endpoint omits them to keep
// responses POSIX-friendly.
static mode_t derive_mode(const struct remote_meta *meta) {
    mode_t mode = (mode_t)meta->mode;
    mode_t type_bits = meta->is_dir ? S_IFDIR : S_IFREG;
    if ((mode & S_IFMT) == 0) {
        mode |= type_bits;
    }
    if ((mode & 0777) == 0) {
        mode |= meta->is_dir ? 0550 : 0440;
    }
    return mode;
}

// hydrate_stat maps remote metadata to the fields expected by the calling
// process, synthesizing inode/timestamps as needed.
static int hydrate_stat(const char *abs_path, const struct remote_meta *meta, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = derive_mode(meta);
    st->st_uid = meta->uid;
    st->st_gid = meta->gid;
    st->st_nlink = meta->is_dir ? 2 : 1;
    st->st_size = (off_t)meta->size;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_dev = 0;
    st->st_ino = (ino_t)(hash_path(abs_path));
    struct timespec now;
    fill_time(&now);
#if defined(__APPLE__)
    st->st_atimespec = now;
    st->st_mtimespec = now;
    st->st_ctimespec = now;
#else
    st->st_atim = now;
    st->st_mtim = now;
    st->st_ctim = now;
#endif
    return 0;
}

#ifdef __USE_LARGEFILE64
// hydrate_stat64 mirrors hydrate_stat for large-file callers.
static int hydrate_stat64(const char *abs_path, const struct remote_meta *meta, struct stat64 *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = derive_mode(meta);
    st->st_uid = meta->uid;
    st->st_gid = meta->gid;
    st->st_nlink = meta->is_dir ? 2 : 1;
    st->st_size = (off64_t)meta->size;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_dev = 0;
    st->st_ino = (ino64_t)(hash_path(abs_path));
    struct timespec now;
    fill_time(&now);
    st->st_atim = now;
    st->st_mtim = now;
    st->st_ctim = now;
    return 0;
}
#endif

#if REMOTEFS_HAVE_STATX
// fill_statx_timestamp/hydrate_statx provide statx-compatible responses based
// on the daemon metadata. Linux-only for now.
static void fill_statx_timestamp(struct statx_timestamp *dst) {
    struct timespec now;
    fill_time(&now);
    dst->tv_sec = (int64_t)now.tv_sec;
    dst->tv_nsec = (uint32_t)now.tv_nsec;
    dst->__reserved = 0;
}

static int hydrate_statx(const char *abs_path, const struct remote_meta *meta, struct statx *stx) {
    if (stx == NULL) {
        errno = EFAULT;
        return -1;
    }
    memset(stx, 0, sizeof(*stx));
    stx->stx_mask = STATX_BASIC_STATS;
    mode_t mode = derive_mode(meta);
    stx->stx_mode = (uint16_t)mode;
    stx->stx_uid = meta->uid;
    stx->stx_gid = meta->gid;
    stx->stx_nlink = meta->is_dir ? 2 : 1;
    stx->stx_size = meta->size;
    stx->stx_blocks = (meta->size + 511) / 512;
    stx->stx_blksize = 4096;
    stx->stx_attributes = 0;
    stx->stx_attributes_mask = 0;
    stx->stx_dev_major = 0;
    stx->stx_dev_minor = 0;
    stx->stx_rdev_major = 0;
    stx->stx_rdev_minor = 0;
    stx->stx_ino = hash_path(abs_path);
    fill_statx_timestamp(&stx->stx_atime);
    fill_statx_timestamp(&stx->stx_mtime);
    fill_statx_timestamp(&stx->stx_ctime);
    return 0;
}
#endif

// fetch_remote_meta issues /stat and decodes the JSON payload into remote_meta.
static int fetch_remote_meta(const char *abs_path, struct remote_meta *meta_out) {
    struct buffer buf = {0};
    long status = 0;
    if (fetch_json("/stat", abs_path, &buf, &status) != 0) {
        return -1;
    }
    if (status == 404) {
        free(buf.data);
        errno = ENOENT;
        return -1;
    }
    if (status != 200) {
        free(buf.data);
        errno = EIO;
        return -1;
    }
    jsmntok_t *tokens = NULL;
    if (parse_json_document(buf.data, &tokens) < 1) {
        free(buf.data);
        free(tokens);
        errno = EIO;
        return -1;
    }
    struct remote_meta meta = {0};
    if (parse_object_at(buf.data, tokens, 0, &meta, NULL) != 0) {
        free(buf.data);
        free(tokens);
        errno = EIO;
        return -1;
    }
    if (meta_out != NULL) {
        *meta_out = meta;
    }
    free(buf.data);
    free(tokens);
    return 0;
}

// remote_stat_path fetches metadata from the daemon and fills struct stat.
static int remote_stat_path(const char *abs_path, struct stat *st) {
    struct remote_meta meta = {0};
    if (fetch_remote_meta(abs_path, &meta) != 0) {
        return -1;
    }
    return hydrate_stat(abs_path, &meta, st);
}

#ifdef __USE_LARGEFILE64
// remote_stat64_path mirrors remote_stat_path for struct stat64.
static int remote_stat64_path(const char *abs_path, struct stat64 *st) {
    struct remote_meta meta = {0};
    if (fetch_remote_meta(abs_path, &meta) != 0) {
        return -1;
    }
    return hydrate_stat64(abs_path, &meta, st);
}
#endif

// allow_open_flags enforces the read-only contract exposed by remotefs.
static bool allow_open_flags(int flags) {
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) {
        errno = EROFS;
        return false;
    }
#ifdef O_TMPFILE
    if (flags & O_TMPFILE) {
        errno = EROFS;
        return false;
    }
#endif
    return true;
}

// download_into_temp streams the remote object into a mkstemp-backed file that
// is later reopened read-only.
static int download_into_temp(const char *abs_path, int fd) {
    struct file_writer writer = {.fd = fd};
    long status = 0;
    int rc = perform_http_request("/cat", abs_path, file_write, &writer, &status);
    if (rc != 0) {
        return -1;
    }
    if (status == 404) {
        errno = ENOENT;
        return -1;
    }
    if (status != 200) {
        errno = EIO;
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}

// remote_open_file validates the path, downloads it once, and reopens it so
// the calling process gets a regular fd backed by local disk.
static int remote_open_file(const char *abs_path, int flags) {
    if (!allow_open_flags(flags)) {
        return -1;
    }
    struct stat st;
    if (remote_stat_path(abs_path, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    char template_path[PATH_MAX];
    snprintf(template_path, sizeof(template_path), "%s/cache-XXXXXX", g_cfg.cache_dir);
    int tmp_fd = mkstemp(template_path);
    if (tmp_fd < 0) {
        return -1;
    }
    if (download_into_temp(abs_path, tmp_fd) != 0) {
        close(tmp_fd);
        unlink(template_path);
        return -1;
    }
    close(tmp_fd);
    int fd = open(template_path, O_RDONLY);
    unlink(template_path);
    return fd;
}

// relative_name trims the parent directory prefix so dirents only expose the
// current level entry name.
static char *relative_name(const char *dir_rel, const char *entry_rel) {
    if (dir_rel == NULL || dir_rel[0] == '\0') {
        const char *slash = strchr(entry_rel, '/');
        if (!slash) {
            return strdup(entry_rel);
        }
        size_t len = (size_t)(slash - entry_rel);
        char *out = malloc(len + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, entry_rel, len);
        out[len] = '\0';
        return out;
    }
    size_t base_len = strlen(dir_rel);
    if (strncmp(entry_rel, dir_rel, base_len) != 0) {
        return strdup(entry_rel);
    }
    const char *rest = entry_rel + base_len;
    if (*rest == '/') {
        rest++;
    }
    const char *slash = strchr(rest, '/');
    size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, rest, len);
    out[len] = '\0';
    return out;
}

// alloc_dirent builds a dirent struct sized exactly for the desired filename.
static struct dirent *alloc_dirent(const char *name, unsigned char dtype, ino_t ino, long offset) {
    size_t len = strlen(name);
    size_t size = offsetof(struct dirent, d_name) + len + 1;
    struct dirent *d = malloc(size);
    if (!d) {
        return NULL;
    }
    memset(d, 0, size);
    d->d_ino = ino;
#if defined(_DIRENT_HAVE_D_OFF)
    d->d_off = offset;
#elif defined(__APPLE__)
    d->d_seekoff = offset;
#endif
    d->d_reclen = (unsigned short)size;
#ifdef _DIRENT_HAVE_D_TYPE
    d->d_type = dtype;
#else
    (void)dtype;
#endif
    memcpy(d->d_name, name, len + 1);
    return d;
}

#ifdef __USE_LARGEFILE64
// alloc_dirent64 mirrors alloc_dirent for dirent64 consumers.
static struct dirent64 *alloc_dirent64(const char *name, unsigned char dtype, ino64_t ino, long offset) {
    size_t len = strlen(name);
    size_t size = offsetof(struct dirent64, d_name) + len + 1;
    struct dirent64 *d = malloc(size);
    if (!d) {
        return NULL;
    }
    memset(d, 0, size);
    d->d_ino = ino;
    d->d_off = offset;
    d->d_reclen = (unsigned short)size;
    d->d_type = dtype;
    memcpy(d->d_name, name, len + 1);
    return d;
}
#endif

// add_entry materializes both dirent flavors so readdir/readdir64/r variants
// can share the same cached listing.
static int add_entry(struct shim_dir *dir, size_t index, const char *name, bool is_dir) {
    unsigned char dtype = is_dir ? DT_DIR : DT_REG;
    char full_path[PATH_MAX];
    if (dir->abs_path && name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
        if (strcmp(dir->abs_path, "/") == 0) {
            snprintf(full_path, sizeof(full_path), "/%s", name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", dir->abs_path, name);
        }
    } else {
        strncpy(full_path, dir->abs_path ? dir->abs_path : "/", sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }
    ino_t ino = (ino_t)hash_path(full_path);
    dir->entries[index].d32 = alloc_dirent(name, dtype, ino, (long)index);
    if (!dir->entries[index].d32) {
        return -1;
    }
#ifdef __USE_LARGEFILE64
    dir->entries[index].d64 = alloc_dirent64(name, dtype, ino, (long)index);
    if (!dir->entries[index].d64) {
        free(dir->entries[index].d32);
        dir->entries[index].d32 = NULL;
        return -1;
    }
#endif
    return 0;
}

// free_dir releases every allocated dirent and book-keeping structure.
static void free_dir(struct shim_dir *dir) {
    if (!dir) {
        return;
    }
    for (size_t i = 0; i < dir->count; i++) {
        free(dir->entries[i].d32);
#ifdef __USE_LARGEFILE64
        free(dir->entries[i].d64);
#endif
    }
    free(dir->entries);
    free(dir->abs_path);
    free(dir->rel_path);
    free(dir);
}

// populate_directory parses the /ls response into a shim_dir containing "."/".."
// and every remote child.
static int populate_directory(const char *abs_path, const char *rel_path, struct shim_dir *dir, const char *json) {
    (void)abs_path;
    jsmntok_t *tokens = NULL;
    int tok_count = parse_json_document(json, &tokens);
    if (tok_count < 1) {
        free(tokens);
        return -1;
    }
    if (tokens[0].type != JSMN_ARRAY) {
        free(tokens);
        return -1;
    }
    int idx = 1;
    int children = tokens[0].size;
    size_t total = (size_t)children + 2;
    dir->entries = calloc(total, sizeof(struct dir_entry_pair));
    if (!dir->entries) {
        free(tokens);
        return -1;
    }
    dir->count = total;
    dir->index = 0;
    if (add_entry(dir, 0, ".", true) != 0) {
        free(tokens);
        return -1;
    }
    if (add_entry(dir, 1, "..", true) != 0) {
        free(tokens);
        return -1;
    }
    size_t slot = 2;
    for (int i = 0; i < children; i++) {
        struct remote_meta meta = {0};
        if (parse_object_at(json, tokens, idx, &meta, &idx) != 0) {
            free(tokens);
            return -1;
        }
        char *name = relative_name(rel_path, meta.path);
        if (!name || name[0] == '\0') {
            free(name);
            continue;
        }
        if (add_entry(dir, slot, name, meta.is_dir) != 0) {
            free(name);
            free(tokens);
            return -1;
        }
        free(name);
        slot++;
    }
    dir->count = slot;
    free(tokens);
    return 0;
}

// register_dir inserts the shim_dir in a global list so later readdir/closedir
// hooks can locate it.
static void register_dir(struct shim_dir *dir) {
    pthread_mutex_lock(&g_dir_lock);
    dir->next = g_dirs;
    g_dirs = dir;
    pthread_mutex_unlock(&g_dir_lock);
}

// unregister_dir removes the shim_dir when closedir runs.
static void unregister_dir(struct shim_dir *dir) {
    pthread_mutex_lock(&g_dir_lock);
    struct shim_dir **cur = &g_dirs;
    while (*cur) {
        if (*cur == dir) {
            *cur = dir->next;
            break;
        }
        cur = &((*cur)->next);
    }
    pthread_mutex_unlock(&g_dir_lock);
}

// lookup_dir hands back a shim_dir when the DIR* originates from the shim.
static struct shim_dir *lookup_dir(DIR *handle) {
    pthread_mutex_lock(&g_dir_lock);
    struct shim_dir *cur = g_dirs;
    while (cur) {
        if ((DIR *)cur == handle) {
            pthread_mutex_unlock(&g_dir_lock);
            return cur;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_dir_lock);
    return NULL;
}

// dir_next_entry steps through cached entries for 32-bit dirent consumers.
static struct dirent *dir_next_entry(struct shim_dir *dir) {
    if (!dir || dir->index >= dir->count) {
        return NULL;
    }
    return dir->entries[dir->index++].d32;
}

#ifdef __USE_LARGEFILE64
// dir_next_entry64 variant for dirent64 iteration.
static struct dirent64 *dir_next_entry64(struct shim_dir *dir) {
    if (!dir || dir->index >= dir->count) {
        return NULL;
    }
    return dir->entries[dir->index++].d64;
}
#endif

// handle_stat is the common entry point for stat-family wrappers.
static int handle_stat(const char *path, struct stat *st) {
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!path_within_root(path, abs_path, sizeof(abs_path), NULL, 0)) {
        return real_stat_fn ? real_stat_fn(path, st) : -1;
    }
    return remote_stat_path(abs_path, st);
}

// handle_lstat currently behaves the same as stat because the remote tree never
// exposes symlinks.
static int handle_lstat(const char *path, struct stat *st) {
    return handle_stat(path, st);
}

int stat(const char *path, struct stat *st) {
    return handle_stat(path, st);
}

int lstat(const char *path, struct stat *st) {
    return handle_lstat(path, st);
}

int __xstat(int ver, const char *path, struct stat *st) {
    (void)ver;
    return handle_stat(path, st);
}

int __lxstat(int ver, const char *path, struct stat *st) {
    (void)ver;
    return handle_lstat(path, st);
}

#ifdef __USE_LARGEFILE64
// handle_stat64 ensures large-file consumers respect shim boundaries.
static int handle_stat64(const char *path, struct stat64 *st) {
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!path_within_root(path, abs_path, sizeof(abs_path), NULL, 0)) {
        return real_stat64_fn ? real_stat64_fn(path, st) : -1;
    }
    return remote_stat64_path(abs_path, st);
}

int stat64(const char *path, struct stat64 *st) {
    return handle_stat64(path, st);
}

int lstat64(const char *path, struct stat64 *st) {
    return handle_stat64(path, st);
}

int __xstat64(int ver, const char *path, struct stat64 *st) {
    (void)ver;
    return handle_stat64(path, st);
}

int __lxstat64(int ver, const char *path, struct stat64 *st) {
    (void)ver;
    return handle_stat64(path, st);
}
#endif

// fstatat resolves dirfd-relative paths before routing to remote_stat_path.
int fstatat(int dirfd, const char *path, struct stat *st, int flag) {
    (void)flag;
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!resolve_absolute(dirfd, path, abs_path, sizeof(abs_path)) || !path_within_root(abs_path, abs_path, sizeof(abs_path), NULL, 0)) {
        return real_fstatat_fn ? real_fstatat_fn(dirfd, path, st, flag) : -1;
    }
    return remote_stat_path(abs_path, st);
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flag) {
    (void)ver;
    return fstatat(dirfd, path, st, flag);
}

#if REMOTEFS_HAVE_STATX
// statx is only present on Linux but follows the same remote metadata hydration
// path.
int statx(int dirfd, const char *path, int flags, unsigned int mask, struct statx *stx) {
    ensure_initialized();
    (void)flags;
    (void)mask;
    char abs_path[PATH_MAX];
    if (!resolve_absolute(dirfd, path, abs_path, sizeof(abs_path)) ||
        !path_within_root(abs_path, abs_path, sizeof(abs_path), NULL, 0)) {
        if (real_statx_fn) {
            return real_statx_fn(dirfd, path, flags, mask, stx);
        }
        errno = ENOSYS;
        return -1;
    }
    struct remote_meta meta = {0};
    if (fetch_remote_meta(abs_path, &meta) != 0) {
        return -1;
    }
    return hydrate_statx(abs_path, &meta, stx);
}
#endif

// handle_open_common centralizes open/open64 so the shim can delegate to the
// original symbol when the path falls outside REMOTEFS_ROOT.
static int handle_open_common(const char *path, int flags, mode_t mode, bool has_mode, open_func fallback) {
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!path_within_root(path, abs_path, sizeof(abs_path), NULL, 0)) {
        if (fallback) {
            if (has_mode) {
                return fallback(path, flags, mode);
            }
            return fallback(path, flags);
        }
        errno = ENOSYS;
        return -1;
    }
    return remote_open_file(abs_path, flags);
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }
    return handle_open_common(path, flags, mode, has_mode, real_open_fn);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }
    return handle_open_common(path, flags, mode, has_mode, real_open64_fn ? real_open64_fn : real_open_fn);
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!resolve_absolute(dirfd, path, abs_path, sizeof(abs_path)) || !path_within_root(abs_path, abs_path, sizeof(abs_path), NULL, 0)) {
        if (real_openat_fn) {
            if (has_mode) {
                return real_openat_fn(dirfd, path, flags, mode);
            }
            return real_openat_fn(dirfd, path, flags);
        }
        errno = ENOSYS;
        return -1;
    }
    return remote_open_file(abs_path, flags);
}

// opendir issues /ls to seed a shim_dir so POSIX directory APIs operate on
// cached entries.
DIR *opendir(const char *path) {
    ensure_initialized();
    char abs_path[PATH_MAX];
    char rel_path[PATH_MAX];
    if (!path_within_root(path, abs_path, sizeof(abs_path), rel_path, sizeof(rel_path))) {
        return real_opendir_fn ? real_opendir_fn(path) : NULL;
    }
    struct buffer buf = {0};
    long status = 0;
    if (fetch_json("/ls", abs_path, &buf, &status) != 0) {
        return NULL;
    }
    if (status == 404) {
        free(buf.data);
        errno = ENOENT;
        return NULL;
    }
    if (status != 200) {
        free(buf.data);
        errno = EIO;
        return NULL;
    }
    struct shim_dir *dir = calloc(1, sizeof(struct shim_dir));
    if (!dir) {
        free(buf.data);
        errno = ENOMEM;
        return NULL;
    }
    dir->abs_path = strdup(abs_path);
    dir->rel_path = strdup(rel_path);
    if (!dir->abs_path || !dir->rel_path) {
        free(buf.data);
        free_dir(dir);
        errno = ENOMEM;
        return NULL;
    }
    if (populate_directory(abs_path, rel_path, dir, buf.data) != 0) {
        free(buf.data);
        free_dir(dir);
        errno = EIO;
        return NULL;
    }
    free(buf.data);
    register_dir(dir);
    return (DIR *)dir;
}

// readdir hands back cached entries and falls through to the real symbol for
// non-shim handles.
struct dirent *readdir(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_readdir_fn ? real_readdir_fn(dirp) : NULL;
    }
    return dir_next_entry(dir);
}

#ifdef __USE_LARGEFILE64
struct dirent64 *readdir64(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_readdir64_fn ? real_readdir64_fn(dirp) : NULL;
    }
    return dir_next_entry64(dir);
}
#endif

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_readdir_r_fn ? real_readdir_r_fn(dirp, entry, result) : EBADF;
    }
    struct dirent *next = dir_next_entry(dir);
    if (!next) {
        if (result) {
            *result = NULL;
        }
        return 0;
    }
    size_t copy_len = offsetof(struct dirent, d_name) + strlen(next->d_name) + 1;
    memcpy(entry, next, copy_len);
    if (result) {
        *result = entry;
    }
    return 0;
}

#ifdef __USE_LARGEFILE64
int readdir64_r(DIR *dirp, struct dirent64 *entry, struct dirent64 **result) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_readdir64_r_fn ? real_readdir64_r_fn(dirp, entry, result) : EBADF;
    }
    struct dirent64 *next = dir_next_entry64(dir);
    if (!next) {
        if (result) {
            *result = NULL;
        }
        return 0;
    }
    size_t copy_len = offsetof(struct dirent64, d_name) + strlen(next->d_name) + 1;
    memcpy(entry, next, copy_len);
    if (result) {
        *result = entry;
    }
    return 0;
}
#endif

// closedir unregisters shim-managed handles and frees cached state.
int closedir(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_closedir_fn ? real_closedir_fn(dirp) : -1;
    }
    unregister_dir(dir);
    free_dir(dir);
    return 0;
}

// rewinddir resets the iteration index inside the shim_dir cache.
void rewinddir(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        if (real_rewinddir_fn) {
            real_rewinddir_fn(dirp);
        }
        return;
    }
    dir->index = 0;
}

// telldir exposes the current index to callers for compatibility.
long telldir(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_telldir_fn ? real_telldir_fn(dirp) : -1;
    }
    return (long)dir->index;
}

// seekdir lets callers reposition within the cached entries.
void seekdir(DIR *dirp, long loc) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        if (real_seekdir_fn) {
            real_seekdir_fn(dirp, loc);
        }
        return;
    }
    if (loc >= 0 && (size_t)loc < dir->count) {
        dir->index = (size_t)loc;
    }
}

// dirfd cannot surface a real descriptor because the directory is synthetic.
int dirfd(DIR *dirp) {
    struct shim_dir *dir = lookup_dir(dirp);
    if (!dir) {
        return real_dirfd_fn ? real_dirfd_fn(dirp) : -1;
    }
    errno = ENOTSUP;
    return -1;
}

// access/faccessat allow read/execute checks but reject writes to enforce
// read-only semantics before the syscall reaches the daemon.
int access(const char *path, int mode) {
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!path_within_root(path, abs_path, sizeof(abs_path), NULL, 0)) {
        return real_access_fn ? real_access_fn(path, mode) : -1;
    }
    if ((mode & W_OK) != 0) {
        errno = EROFS;
        return -1;
    }
    struct stat st;
    if (remote_stat_path(abs_path, &st) != 0) {
        return -1;
    }
    return 0;
}

// faccessat mirrors access while resolving dirfd-relative paths.
int faccessat(int dirfd, const char *path, int mode, int flags) {
    (void)flags;
    ensure_initialized();
    char abs_path[PATH_MAX];
    if (!resolve_absolute(dirfd, path, abs_path, sizeof(abs_path)) || !path_within_root(abs_path, abs_path, sizeof(abs_path), NULL, 0)) {
        return real_faccessat_fn ? real_faccessat_fn(dirfd, path, mode, flags) : -1;
    }
    if ((mode & W_OK) != 0) {
        errno = EROFS;
        return -1;
    }
    struct stat st;
    if (remote_stat_path(abs_path, &st) != 0) {
        return -1;
    }
    return 0;
}
