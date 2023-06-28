/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include "queue.h"

/*
 * This storage source implementation is used for demonstration and testing. All objects are stored
 * as local files in a designated directory.
 */

#ifdef __GNUC__
#if __GNUC__ > 7 || (__GNUC__ == 7 && __GNUC_MINOR__ > 0)
/*
 * !!!
 * GCC with -Wformat-truncation complains about calls to snprintf in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#endif

/* Directory storage source structure. */
typedef struct {
    WT_STORAGE_SOURCE storage_source; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */

    /*
     * Locks are used to protect the file handle queue and flush queue.
     */
    pthread_rwlock_t file_handle_lock;

    /*
     * Keep the number of references to this storage source.
     */
    uint32_t reference_count;

    /*
     * Configuration values are set at startup.
     */
    uint32_t cache;       /* This flag determines whether or not we cache the file locally. */
    uint32_t delay_ms;    /* Average length of delay when simulated */
    uint32_t error_ms;    /* Average length of sleep when simulated */
    uint32_t force_delay; /* Force a simulated network delay every N operations */
    uint32_t force_error; /* Force a simulated network error every N operations */
    uint32_t verbose;     /* Verbose level */

    /*
     * Statistics are collected but not yet exposed.
     */
    uint64_t fh_ops;        /* Non-read/write operations in file handles */
    uint64_t object_writes; /* (What would be) writes to the cloud */
    uint64_t object_reads;  /* (What would be) reads to the cloud */
    uint64_t op_count;      /* Number of operations done on dir_store */
    uint64_t read_ops;
    uint64_t write_ops;

    /* Queue of file handles */
    TAILQ_HEAD(dir_store_file_handle_qh, dir_store_file_handle) fileq;

} DIR_STORE;

typedef struct {
    /* Must come first - this is the interface for the file system we are implementing. */
    WT_FILE_SYSTEM file_system;
    DIR_STORE *dir_store;

    /* This is WiredTiger's file system, it is used in implementing the dir_store file system. */
    WT_FILE_SYSTEM *wt_fs;

    char *auth_token;     /* Identifier for key management system */
    char *bucket_dir;     /* Directory that stands in for cloud storage bucket */
    char *cache_dir;      /* Directory for cached objects */
    const char *home_dir; /* Owned by the connection */
} DIR_STORE_FILE_SYSTEM;

typedef struct dir_store_file_handle {
    WT_FILE_HANDLE iface; /* Must come first */

    DIR_STORE *dir_store; /* Enclosing storage source */
    WT_FILE_HANDLE *fh;   /* File handle */

    TAILQ_ENTRY(dir_store_file_handle) q; /* Queue of handles */
} DIR_STORE_FILE_HANDLE;

/*
 * Forward function declarations for internal functions
 */
static int dir_store_bucket_path(WT_FILE_SYSTEM *, const char *, char **);
static int dir_store_cache_path(WT_FILE_SYSTEM *, const char *, char **);
static int dir_store_home_path(WT_FILE_SYSTEM *, const char *, char **);
static int dir_store_configure(DIR_STORE *, WT_CONFIG_ARG *);
static int dir_store_configure_int(DIR_STORE *, WT_CONFIG_ARG *, const char *, uint32_t *);
static int dir_store_delay(DIR_STORE *);
static int dir_store_err(DIR_STORE *, WT_SESSION *, int, const char *, ...);
static int dir_store_file_copy(
  DIR_STORE *, WT_SESSION *, const char *, const char *, WT_FS_OPEN_FILE_TYPE, bool);
static int dir_store_get_directory(const char *, const char *, ssize_t len, bool, char **);
static int dir_store_path(WT_FILE_SYSTEM *, const char *, const char *, char **);
static int dir_store_stat(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, bool, struct stat *);

/*
 * Forward function declarations for storage source API implementation
 */
static int dir_store_add_reference(WT_STORAGE_SOURCE *);
static int dir_store_customize_file_system(
  WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *, const char *, WT_FILE_SYSTEM **);
static int dir_store_flush(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *, const char *, const char *);
static int dir_store_flush_finish(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *, const char *, const char *);
static int dir_store_terminate(WT_STORAGE_SOURCE *, WT_SESSION *);

/*
 * Forward function declarations for file system API implementation
 */
static int dir_store_directory_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int dir_store_directory_list_add(DIR_STORE *, char ***, const char *, uint32_t, uint32_t *);
static int dir_store_directory_list_internal(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t, char ***, uint32_t *);
static int dir_store_directory_list_single(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int dir_store_directory_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int dir_store_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int dir_store_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);
static int dir_store_open(WT_FILE_SYSTEM *, WT_SESSION *, const char *,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t, WT_FILE_HANDLE **);
static int dir_store_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int dir_store_remove_if_exists(WT_FILE_SYSTEM *, WT_SESSION *, char *, uint32_t);
static int dir_store_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int dir_store_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);

/*
 * Forward function declarations for file handle API implementation
 */
static int dir_store_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int dir_store_file_close_internal(DIR_STORE *, WT_SESSION *, DIR_STORE_FILE_HANDLE *);
static int dir_store_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int dir_store_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int dir_store_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int dir_store_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int dir_store_file_write(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);

#define FS2DS(fs) (((DIR_STORE_FILE_SYSTEM *)(fs))->dir_store)
#define SHOW_STRING(s) (((s) == NULL) ? "<null>" : (s))
#define VERBOSE_LS(dir_store, ...)        \
    do {                                  \
        if ((dir_store)->verbose > 0)     \
            fprintf(stderr, __VA_ARGS__); \
    } while (0);

/*
 * dir_store_configure
 *     Parse the configuration for the keys we care about.
 */
static int
dir_store_configure(DIR_STORE *dir_store, WT_CONFIG_ARG *config)
{
    int ret;

    if ((ret = dir_store_configure_int(dir_store, config, "cache", &dir_store->cache)) != 0)
        return (ret);
    if ((ret = dir_store_configure_int(dir_store, config, "delay_ms", &dir_store->delay_ms)) != 0)
        return (ret);
    if ((ret = dir_store_configure_int(dir_store, config, "error_ms", &dir_store->error_ms)) != 0)
        return (ret);
    if ((ret = dir_store_configure_int(
           dir_store, config, "force_delay", &dir_store->force_delay)) != 0)
        return (ret);
    if ((ret = dir_store_configure_int(
           dir_store, config, "force_error", &dir_store->force_error)) != 0)
        return (ret);
    if ((ret = dir_store_configure_int(dir_store, config, "verbose", &dir_store->verbose)) != 0)
        return (ret);

    return (0);
}

/*
 * dir_store_configure_int
 *     Look for a particular configuration key, and return its integer value.
 */
static int
dir_store_configure_int(
  DIR_STORE *dir_store, WT_CONFIG_ARG *config, const char *key, uint32_t *valuep)
{
    WT_CONFIG_ITEM v;
    int ret;

    ret = 0;

    if ((ret = dir_store->wt_api->config_get(dir_store->wt_api, NULL, config, key, &v)) == 0) {
        if (v.len == 0 || v.type != WT_CONFIG_ITEM_NUM)
            ret =
              dir_store_err(dir_store, NULL, EINVAL, "force_error config arg: integer required");
        else
            *valuep = (uint32_t)v.val;
    } else if (ret == WT_NOTFOUND)
        ret = 0;
    else
        ret = dir_store_err(dir_store, NULL, EINVAL, "WT_API->config_get");

    return (ret);
}

/*
 * sleep_ms --
 *     Sleep for the specified milliseconds.
 */
static void
sleep_ms(uint32_t ms)
{
    struct timeval tv;

    /* Cast needed for some compilers that suspect the calculation can overflow (it can't). */
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
    (void)select(0, NULL, NULL, NULL, &tv);
}

/*
 * dir_store_delay --
 *     Add any artificial delay or simulated network error during an object transfer.
 */
static int
dir_store_delay(DIR_STORE *dir_store)
{
    int ret;

    ret = 0;
    if (dir_store->force_delay != 0 &&
      (dir_store->object_reads + dir_store->object_writes) % dir_store->force_delay == 0) {
        VERBOSE_LS(dir_store,
          "Artificial delay %" PRIu32 " milliseconds after %" PRIu64 " object reads, %" PRIu64
          " object writes\n",
          dir_store->delay_ms, dir_store->object_reads, dir_store->object_writes);
        sleep_ms(dir_store->delay_ms);
    }
    if (dir_store->force_error != 0 &&
      (dir_store->object_reads + dir_store->object_writes) % dir_store->force_error == 0) {
        VERBOSE_LS(dir_store,
          "Artificial error returned after %" PRIu32 " milliseconds sleep, %" PRIu64
          " object reads, %" PRIu64 " object writes\n",
          dir_store->error_ms, dir_store->object_reads, dir_store->object_writes);

        sleep_ms(dir_store->error_ms);
        ret = ENETUNREACH;
    }

    return (ret);
}

/*
 * dir_store_err --
 *     Print errors from the interface. Returns "ret", the third argument.
 */
static int
dir_store_err(DIR_STORE *dir_store, WT_SESSION *session, int ret, const char *format, ...)
{
    va_list ap;
    WT_EXTENSION_API *wt_api;
    char buf[1000];

    va_start(ap, format);
    wt_api = dir_store->wt_api;
    if (vsnprintf(buf, sizeof(buf), format, ap) >= (int)sizeof(buf))
        wt_api->err_printf(wt_api, session, "dir_store: error overflow");
    wt_api->err_printf(
      wt_api, session, "dir_store: %s: %s", wt_api->strerror(wt_api, session, ret), buf);
    va_end(ap);

    return (ret);
}

/*
 * dir_store_get_directory --
 *     Return a copy of a directory name after verifying that it is a directory.
 */
static int
dir_store_get_directory(const char *home, const char *s, ssize_t len, bool create, char **copy)
{
    struct stat sb;
    size_t buflen;
    int ret;
    char *dirname;

    *copy = NULL;

    if (len == -1)
        len = (ssize_t)strlen(s);

    /* For relative pathnames, the path is considered to be relative to the home directory. */
    if (*s == '/')
        dirname = strndup(s, (size_t)len + 1); /* Room for null */
    else {
        buflen = (size_t)len + strlen(home) + 2; /* Room for slash, null */
        if ((dirname = malloc(buflen)) != NULL) {
            if (snprintf(dirname, buflen, "%s/%.*s", home, (int)len, s) >= (int)buflen) {
                free(dirname);
                return (EINVAL);
            }
        }
    }
    if (dirname == NULL)
        return (ENOMEM);

    ret = stat(dirname, &sb);
    if (ret != 0 && errno == ENOENT && create) {
        (void)mkdir(dirname, 0777);
        ret = stat(dirname, &sb);
    }
    if (ret != 0)
        ret = errno;
    else if ((sb.st_mode & S_IFMT) != S_IFDIR)
        ret = EINVAL;
    if (ret != 0)
        free(dirname);
    else
        *copy = dirname;
    return (ret);
}

/*
 * dir_store_bucket_path --
 *     Construct the bucket pathname from the file system and dir_store name.
 */
static int
dir_store_bucket_path(WT_FILE_SYSTEM *file_system, const char *name, char **pathp)
{
    return (
      dir_store_path(file_system, ((DIR_STORE_FILE_SYSTEM *)file_system)->bucket_dir, name, pathp));
}

/*
 * dir_store_cache_path --
 *     Construct the cache pathname from the file system and dir_store name.
 */
static int
dir_store_cache_path(WT_FILE_SYSTEM *file_system, const char *name, char **pathp)
{
    return (
      dir_store_path(file_system, ((DIR_STORE_FILE_SYSTEM *)file_system)->cache_dir, name, pathp));
}

/*
 * dir_store_home_path --
 *     Construct the source pathname from the file system and dir_store name.
 */
static int
dir_store_home_path(WT_FILE_SYSTEM *file_system, const char *name, char **pathp)
{
    return (
      dir_store_path(file_system, ((DIR_STORE_FILE_SYSTEM *)file_system)->home_dir, name, pathp));
}

/*
 * dir_store_path --
 *     Construct a pathname from the file system and dir_store name.
 */
static int
dir_store_path(WT_FILE_SYSTEM *file_system, const char *dir, const char *name, char **pathp)
{
    size_t len;
    int ret;
    char *p;

    ret = 0;

    /* Skip over "./" and variations (".//", ".///./././//") at the beginning of the name. */
    while (*name == '.') {
        if (name[1] != '/')
            break;
        name += 2;
        while (*name == '/')
            name++;
    }
    len = strlen(dir) + strlen(name) + 2;
    if ((p = malloc(len)) == NULL)
        return (dir_store_err(FS2DS(file_system), NULL, ENOMEM, "dir_store_path"));
    if (snprintf(p, len, "%s/%s", dir, name) >= (int)len) {
        free(p);
        return (dir_store_err(FS2DS(file_system), NULL, EINVAL, "overflow snprintf"));
    }
    *pathp = p;
    return (ret);
}

/*
 * dir_store_stat --
 *     Perform the stat system call for a name in the file system.
 */
static int
dir_store_stat(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  const char *caller, bool must_exist, struct stat *statp)
{
    int ret;
    char *path;

    path = NULL;

    /*
     * We check to see if the file exists in the cache first, and if not the bucket directory. This
     * maps what a real cloud implementation would do. This will allow us to instrument this code to
     * try out and measure caching implementations.
     */
    if ((ret = dir_store_cache_path(file_system, name, &path)) != 0)
        goto err;

    ret = stat(path, statp);
    if (ret != 0 && errno == ENOENT) {
        /* It's not in the cache, try the bucket directory. */
        free(path);
        path = NULL;
        if ((ret = dir_store_bucket_path(file_system, name, &path)) != 0)
            goto err;
        ret = stat(path, statp);
    }
    if (ret != 0) {
        /*
         * If the file must exist, report the error no matter what.
         */
        if (must_exist || errno != ENOENT)
            ret = dir_store_err(FS2DS(file_system), session, errno, "%s: %s stat", path, caller);
        else
            ret = errno;
    }
err:
    free(path);
    return (ret);
}

/*
 * dir_store_add_reference --
 *     Add a reference to the storage source so we can reference count to know when to really
 *     terminate.
 */
static int
dir_store_add_reference(WT_STORAGE_SOURCE *storage_source)
{
    DIR_STORE *dir_store;

    dir_store = (DIR_STORE *)storage_source;

    /*
     * Missing reference or overflow?
     */
    if (dir_store->reference_count == 0 || dir_store->reference_count + 1 == 0)
        return (EINVAL);
    ++dir_store->reference_count;
    return (0);
}

/*
 * dir_store_customize_file_system --
 *     Return a customized file system to access the dir_store storage source objects.
 */
static int
dir_store_customize_file_system(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *bucket_name, const char *auth_token, const char *config,
  WT_FILE_SYSTEM **file_systemp)
{
    DIR_STORE *dir_store;
    DIR_STORE_FILE_SYSTEM *fs;
    WT_CONFIG_ITEM cachedir;
    WT_FILE_SYSTEM *wt_fs;
    int ret;
    const char *p;
    char buf[1024];

    dir_store = (DIR_STORE *)storage_source;

    fs = NULL;
    ret = 0;

    /* Parse configuration string. */
    if ((ret = dir_store->wt_api->config_get_string(
           dir_store->wt_api, session, config, "cache_directory", &cachedir)) != 0) {
        if (ret == WT_NOTFOUND) {
            ret = 0;
            cachedir.len = 0;
        } else {
            ret = dir_store_err(dir_store, session, ret, "customize_file_system: config parsing");
            goto err;
        }
    }

    if ((ret = dir_store->wt_api->file_system_get(dir_store->wt_api, session, &wt_fs)) != 0) {
        ret = dir_store_err(
          dir_store, session, ret, "dir_store_file_system: cannot get WiredTiger file system");
        goto err;
    }
    if ((fs = calloc(1, sizeof(DIR_STORE_FILE_SYSTEM))) == NULL) {
        ret = dir_store_err(dir_store, session, ENOMEM, "dir_store_file_system");
        goto err;
    }
    fs->dir_store = dir_store;
    fs->wt_fs = wt_fs;

    if ((fs->auth_token = strdup(auth_token)) == NULL) {
        ret = dir_store_err(dir_store, session, ENOMEM, "dir_store_file_system.auth_token");
        goto err;
    }

    /*
     * The home directory owned by the connection will not change, and will be valid memory, for as
     * long as the connection is open. That is longer than this file system will be open, so we can
     * use the string without copying.
     */
    fs->home_dir = session->connection->get_home(session->connection);

    /*
     * Get the bucket directory and the cache directory.
     */
    if ((ret = dir_store_get_directory(fs->home_dir, bucket_name, -1, false, &fs->bucket_dir)) !=
      0) {
        ret = dir_store_err(dir_store, session, ret, "%s: bucket directory", bucket_name);
        goto err;
    }

    /*
     * The default cache directory is named "cache-<name>", where name is the last component of the
     * bucket name's path. We'll create it if it doesn't exist.
     */
    if (cachedir.len == 0) {
        if ((p = strrchr(bucket_name, '/')) != NULL)
            p++;
        else
            p = bucket_name;
        if (snprintf(buf, sizeof(buf), "cache-%s", p) >= (int)sizeof(buf)) {
            ret = dir_store_err(dir_store, session, EINVAL, "overflow snprintf");
            goto err;
        }
        cachedir.str = buf;
        cachedir.len = strlen(buf);
    }
    if ((ret = dir_store_get_directory(
           fs->home_dir, cachedir.str, (ssize_t)cachedir.len, true, &fs->cache_dir)) != 0) {
        ret = dir_store_err(
          dir_store, session, ret, "%*s: cache directory", (int)cachedir.len, cachedir.str);
        goto err;
    }

    fs->file_system.fs_directory_list = dir_store_directory_list;
    fs->file_system.fs_directory_list_single = dir_store_directory_list_single;
    fs->file_system.fs_directory_list_free = dir_store_directory_list_free;
    fs->file_system.fs_exist = dir_store_exist;
    fs->file_system.fs_open_file = dir_store_open;
    fs->file_system.fs_remove = dir_store_remove;
    fs->file_system.fs_rename = dir_store_rename;
    fs->file_system.fs_size = dir_store_size;
    fs->file_system.terminate = dir_store_fs_terminate;

err:
    if (ret == 0)
        *file_systemp = &fs->file_system;
    else if (fs != NULL) {
        free(fs->auth_token);
        free(fs->bucket_dir);
        free(fs->cache_dir);
        free(fs);
    }
    return (ret);
}

/*
 * dir_store_exist --
 *     Return if the file exists.
 */
static int
dir_store_exist(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    struct stat sb;
    DIR_STORE *dir_store;
    int ret;

    dir_store = FS2DS(file_system);
    dir_store->op_count++;
    *existp = false;

    if ((ret = dir_store_stat(file_system, session, name, "ss_exist", false, &sb)) == 0)
        *existp = true;
    else if (ret == ENOENT)
        ret = 0;

    return (ret);
}

/*
 * dir_store_file_copy --
 *     Copy a file.
 */
static int
dir_store_file_copy(DIR_STORE *dir_store, WT_SESSION *session, const char *src_path,
  const char *dest_path, WT_FS_OPEN_FILE_TYPE type, bool enoent_okay)
{
    WT_FILE_HANDLE *dest, *src;
    WT_FILE_SYSTEM *wt_fs;
    wt_off_t copy_size, file_size, left;
    ssize_t pos;
    size_t pathlen;
    int ret, t_ret;
    char buffer[1024 * 64], *tmp_path;

    dest = src = NULL;
    pathlen = strlen(dest_path) + 10;
    if ((tmp_path = malloc(pathlen)) != NULL)
        if (snprintf(tmp_path, pathlen, "%s.TMP", dest_path) >= (int)pathlen) {
            ret = dir_store_err(dir_store, session, EINVAL, "overflow snprintf");
            goto err;
        }

    if (tmp_path == NULL)
        return (ENOMEM);

    if ((ret = dir_store->wt_api->file_system_get(dir_store->wt_api, session, &wt_fs)) != 0) {
        ret = dir_store_err(
          dir_store, session, ret, "dir_store_file_system: cannot get WiredTiger file system");
        goto err;
    }
    if ((ret = wt_fs->fs_open_file(wt_fs, session, src_path, type, WT_FS_OPEN_READONLY, &src)) !=
      0) {
        /*
         * It is normal and possible that the source file was dropped. Don't print out an error
         * message in that case, but still return the ENOENT error value.
         */
        if ((ret == ENOENT && !enoent_okay) || (ret != 0))
            ret = dir_store_err(dir_store, session, ret, "%s: cannot open for read", src_path);
        goto err;
    }

    if ((ret = wt_fs->fs_open_file(wt_fs, session, tmp_path, type, WT_FS_OPEN_CREATE, &dest)) !=
      0) {
        ret = dir_store_err(dir_store, session, ret, "%s: cannot create", tmp_path);
        goto err;
    }
    if ((ret = wt_fs->fs_size(wt_fs, session, src_path, &file_size)) != 0) {
        ret = dir_store_err(dir_store, session, ret, "%s: cannot get size", src_path);
        goto err;
    }
    for (pos = 0, left = file_size; left > 0; pos += copy_size, left -= copy_size) {
        copy_size = left < (wt_off_t)sizeof(buffer) ? left : (wt_off_t)sizeof(buffer);
        if ((ret = src->fh_read(src, session, pos, (size_t)copy_size, buffer)) != 0) {
            ret = dir_store_err(dir_store, session, ret, "%s: cannot read", src_path);
            goto err;
        }
        if ((ret = dest->fh_write(dest, session, pos, (size_t)copy_size, buffer)) != 0) {
            ret = dir_store_err(dir_store, session, ret, "%s: cannot write", tmp_path);
            goto err;
        }
    }
    if (ret == 0 && (ret = chmod(tmp_path, 0444)) < 0)
        ret = dir_store_err(dir_store, session, errno, "%s: file_copy chmod failed", tmp_path);
    if ((ret = rename(tmp_path, dest_path)) != 0) {
        ret = dir_store_err(
          dir_store, session, errno, "%s: cannot rename from %s", dest_path, tmp_path);
        goto err;
    }
err:
    if (src != NULL && (t_ret = src->close(src, session)) != 0)
        if (ret == 0)
            ret = t_ret;
    if (dest != NULL && (t_ret = dest->close(dest, session)) != 0)
        if (ret == 0)
            ret = t_ret;
    if (ret != 0)
        (void)unlink(tmp_path);
    free(tmp_path);

    return (ret);
}

/*
 * dir_store_flush --
 *     Return when the file has been flushed.
 */
static int
dir_store_flush(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
  const char *source, const char *object, const char *config)
{
    DIR_STORE *dir_store;
    int ret;
    char *dest_path, *src_path;

    (void)config; /* unused */
    dest_path = src_path = NULL;
    dir_store = (DIR_STORE *)storage_source;
    ret = 0;

    if (file_system == NULL || source == NULL || object == NULL)
        return dir_store_err(dir_store, session, EINVAL, "ss_flush: required arguments missing");

    if ((ret = dir_store_home_path(file_system, source, &src_path)) != 0)
        goto err;

    if ((ret = dir_store_bucket_path(file_system, object, &dest_path)) != 0)
        goto err;

    if ((ret = dir_store_delay(dir_store)) != 0)
        goto err;

    if ((ret = dir_store_file_copy(
           dir_store, session, src_path, dest_path, WT_FS_OPEN_FILE_TYPE_DATA, true)) != 0)
        goto err;

    dir_store->object_writes++;

err:
    free(dest_path);
    free(src_path);
    return (ret);
}

/*
 * dir_store_flush_finish --
 *     Cache a file in the new file system.
 */
static int
dir_store_flush_finish(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_FILE_SYSTEM *file_system, const char *source, const char *object, const char *config)
{
    DIR_STORE *dir_store;
    int ret;
    char *dest_path, *src_path;

    (void)config; /* unused */
    dest_path = src_path = NULL;
    dir_store = (DIR_STORE *)storage_source;
    ret = 0;

    if (dir_store->cache == 0)
        return (0);

    if (file_system == NULL || source == NULL || object == NULL)
        return dir_store_err(
          dir_store, session, EINVAL, "ss_flush_finish: required arguments missing");

    if ((ret = dir_store_home_path(file_system, source, &src_path)) != 0)
        goto err;

    if ((ret = dir_store_cache_path(file_system, object, &dest_path)) != 0)
        goto err;

    dir_store->op_count++;
    /*
     * Link the object with the original dir_store object. The could be replaced by a file copy if
     * portability is an issue.
     */
    if ((ret = link(src_path, dest_path)) != 0) {
        ret = dir_store_err(
          dir_store, session, errno, "ss_flush_finish link %s to %s failed", source, dest_path);
        goto err;
    }
    /* Set the file to readonly in the cache. */
    if ((ret = chmod(dest_path, 0444)) < 0)
        ret =
          dir_store_err(dir_store, session, errno, "%s: ss_flush_finish chmod failed", dest_path);
err:
    free(dest_path);
    free(src_path);
    return (ret);
}

/*
 * dir_store_directory_list --
 *     Return a list of object names for the given location.
 */
static int
dir_store_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    FS2DS(file_system)->op_count++;
    return (dir_store_directory_list_internal(
      file_system, session, directory, prefix, 0, dirlistp, countp));
}

/*
 * dir_store_directory_list_single --
 *     Return a single file name for the given location.
 */
static int
dir_store_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    FS2DS(file_system)->op_count++;
    return (dir_store_directory_list_internal(
      file_system, session, directory, prefix, 1, dirlistp, countp));
}

/*
 * dir_store_location_list_free --
 *     Free memory allocated by dir_store_location_list.
 */
static int
dir_store_directory_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    (void)session;

    FS2DS(file_system)->op_count++;
    if (dirlist != NULL) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }
    return (0);
}

/*
 * dir_store_directory_list_add --
 *     Add an entry to the directory list, growing as needed.
 */
static int
dir_store_directory_list_add(
  DIR_STORE *dir_store, char ***entriesp, const char *s, uint32_t count, uint32_t *allocatedp)
{
    size_t alloc_sz;
    char **entries, **new_entries;

    entries = *entriesp;
    if (count >= *allocatedp) {
        *allocatedp += 10;
        alloc_sz = sizeof(char *) * (*allocatedp);
        if ((new_entries = realloc(entries, alloc_sz)) == NULL)
            return (dir_store_err(dir_store, NULL, ENOMEM, "cannot grow directory list"));
        entries = new_entries;
        *entriesp = entries;
    }
    if ((entries[count] = strdup(s)) == NULL)
        return (dir_store_err(dir_store, NULL, ENOMEM, "cannot grow directory list"));

    return (0);
}

/*
 * dir_store_location_list_internal --
 *     Return a list of object names for the given location.
 */
static int
dir_store_directory_list_internal(WT_FILE_SYSTEM *file_system, WT_SESSION *session,
  const char *directory, const char *prefix, uint32_t limit, char ***dirlistp, uint32_t *countp)
{
    struct dirent *dp;
    DIR *dirp;
    DIR_STORE_FILE_SYSTEM *dir_store_fs;
    DIR_STORE *dir_store;
    size_t dir_len, prefix_len;
    uint32_t allocated, count;
    int ret, t_ret;
    char **entries;
    const char *basename;

    dir_store_fs = (DIR_STORE_FILE_SYSTEM *)file_system;
    dir_store = dir_store_fs->dir_store;
    entries = NULL;
    allocated = count = 0;
    dir_len = (directory == NULL ? 0 : strlen(directory));
    prefix_len = (prefix == NULL ? 0 : strlen(prefix));
    ret = 0;

    *dirlistp = NULL;
    *countp = 0;

    /*
     * The items in the bucket directory represent the definitive list.
     */
    if ((dirp = opendir(dir_store_fs->bucket_dir)) == NULL) {
        ret = errno;
        if (ret == 0)
            ret = EINVAL;
        return (dir_store_err(
          dir_store, session, ret, "%s: ss_directory_list: opendir", dir_store_fs->bucket_dir));
    }

    for (count = 0; (dp = readdir(dirp)) != NULL && (limit == 0 || count < limit);) {
        /* Skip . and .. */
        basename = dp->d_name;
        if (strcmp(basename, ".") == 0 || strcmp(basename, "..") == 0)
            continue;

        /* Match only the indicated directory files. */
        if (directory != NULL && strncmp(basename, directory, dir_len) != 0)
            continue;
        basename += dir_len;

        /* The list of files is optionally filtered by a prefix. */
        if (prefix != NULL && strncmp(basename, prefix, prefix_len) != 0)
            continue;

        if ((ret = dir_store_directory_list_add(
               dir_store, &entries, basename, count, &allocated)) != 0)
            goto err;
        count++;
    }

    *dirlistp = entries;
    *countp = count;

err:
    if (closedir(dirp) != 0) {
        t_ret = dir_store_err(
          dir_store, session, errno, "%s: ss_directory_list: closedir", dir_store_fs->cache_dir);
        if (ret == 0)
            ret = t_ret;
    }
    if (ret == 0)
        return (0);

    if (entries != NULL) {
        while (count > 0)
            free(entries[--count]);
        free(entries);
    }
    return (ret);
}

/*
 * dir_store_fs_terminate --
 *     Discard any resources on termination of the file system
 */
static int
dir_store_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    DIR_STORE_FILE_SYSTEM *dir_store_fs;

    (void)session; /* unused */

    dir_store_fs = (DIR_STORE_FILE_SYSTEM *)file_system;
    FS2DS(file_system)->op_count++;
    free(dir_store_fs->auth_token);
    free(dir_store_fs->bucket_dir);
    free(dir_store_fs->cache_dir);
    free(file_system);

    return (0);
}

/*
 * dir_store_open --
 *     fopen for our dir_store storage source
 */
static int
dir_store_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    DIR_STORE_FILE_HANDLE *dir_store_fh;
    DIR_STORE_FILE_SYSTEM *dir_store_fs;
    DIR_STORE *dir_store;
    WT_FILE_HANDLE *file_handle, *wt_fh;
    WT_FILE_SYSTEM *wt_fs;
    struct stat sb;
    int ret;
    char *bucket_path, *cache_path;

    ret = 0;
    *file_handlep = NULL;
    dir_store_fh = NULL;
    dir_store_fs = (DIR_STORE_FILE_SYSTEM *)file_system;
    dir_store = dir_store_fs->dir_store;
    wt_fs = dir_store_fs->wt_fs;
    bucket_path = cache_path = NULL;

    if ((flags & WT_FS_OPEN_READONLY) == 0 || (flags & WT_FS_OPEN_CREATE) != 0)
        return (dir_store_err(
          dir_store, session, EINVAL, "ss_open_object: readonly access required: %s", name));

    /*
     * We expect that the dir_store file system will be used narrowly, like when creating or opening
     * a data file or turtle file. It would be unexpected to try to open a non-data file (like a log
     * file) in that narrow part of code, so we make it an error here.
     *
     * Relaxing this constraint to allow opening of, say, log files, would be straightforward - we
     * would not translate the path or do any tracking for flushing. But there's a catch. Other
     * parts of the API, like remove and rename, have no flag indicating that they are operating on
     * a log file, so we wouldn't know whether to do path translation. Of course, we could peek at
     * the name, but that would be bad form.
     */
    if (file_type != WT_FS_OPEN_FILE_TYPE_DATA && file_type != WT_FS_OPEN_FILE_TYPE_REGULAR)
        return (dir_store_err(dir_store, session, EINVAL,
          "%s: open: only data file and regular types supported", name));

    /* Create a new handle. */
    if ((dir_store_fh = calloc(1, sizeof(DIR_STORE_FILE_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }
    if (dir_store->cache != 0) {
        if ((ret = dir_store_cache_path(file_system, name, &cache_path)) != 0)
            goto err;
        ret = stat(cache_path, &sb);
        if (ret != 0) {
            if (errno != ENOENT) {
                ret =
                  dir_store_err(dir_store, session, errno, "%s: dir_store_open stat", cache_path);
                goto err;
            }

            /*
             * The file doesn't exist locally, make a copy of it from the cloud.
             */
            if ((ret = dir_store_bucket_path(file_system, name, &bucket_path)) != 0)
                goto err;

            if ((ret = dir_store_delay(dir_store)) != 0)
                goto err;

            if ((ret = dir_store_file_copy(dir_store, session, bucket_path, cache_path,
                   WT_FS_OPEN_FILE_TYPE_DATA, false)) != 0)
                goto err;
        }
        if ((ret = wt_fs->fs_open_file(wt_fs, session, cache_path, file_type, flags, &wt_fh)) !=
          0) {
            ret = dir_store_err(dir_store, session, ret, "ss_open_object: open: %s", name);
            goto err;
        }
    } else {
        if ((ret = dir_store_bucket_path(file_system, name, &bucket_path)) != 0)
            goto err;
        ret = stat(bucket_path, &sb);
        if (ret != 0) {
            ret = dir_store_err(dir_store, session, errno, "%s: dir_store_open stat", bucket_path);
            goto err;
        }
        if ((ret = wt_fs->fs_open_file(wt_fs, session, bucket_path, file_type, flags, &wt_fh)) !=
          0) {
            ret = dir_store_err(dir_store, session, ret, "ss_open_object: open: %s", name);
            goto err;
        }
    }

    dir_store->object_reads++;
    dir_store_fh->fh = wt_fh;
    dir_store_fh->dir_store = dir_store;

    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)dir_store_fh;

    /*
     * Setup the function call table for our custom storage source. Set the function pointer to NULL
     * where our implementation doesn't support the functionality.
     */
    file_handle->close = dir_store_file_close;
    file_handle->fh_advise = NULL;
    file_handle->fh_extend = NULL;
    file_handle->fh_extend_nolock = NULL;
    file_handle->fh_lock = dir_store_file_lock;
    file_handle->fh_map = NULL;
    file_handle->fh_map_discard = NULL;
    file_handle->fh_map_preload = NULL;
    file_handle->fh_unmap = NULL;
    file_handle->fh_read = dir_store_file_read;
    file_handle->fh_size = dir_store_file_size;
    file_handle->fh_sync = dir_store_file_sync;
    file_handle->fh_sync_nowait = NULL;
    file_handle->fh_truncate = NULL;
    file_handle->fh_write = dir_store_file_write;
    if ((file_handle->name = strdup(name)) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    if ((ret = pthread_rwlock_wrlock(&dir_store->file_handle_lock)) != 0) {
        (void)dir_store_err(dir_store, session, ret, "ss_open_object: pthread_rwlock_wrlock");
        goto err;
    }
    TAILQ_INSERT_HEAD(&dir_store->fileq, dir_store_fh, q);
    if ((ret = pthread_rwlock_unlock(&dir_store->file_handle_lock)) != 0) {
        (void)dir_store_err(dir_store, session, ret, "ss_open_object: pthread_rwlock_unlock");
        goto err;
    }

    *file_handlep = file_handle;

    VERBOSE_LS(dir_store, "File opened: %s final path=%s\n", SHOW_STRING(name),
      SHOW_STRING(dir_store_fh->fh->name));

err:
    free(bucket_path);
    if (cache_path != NULL)
        free(cache_path);
    if (ret != 0) {
        if (dir_store_fh != NULL)
            dir_store_file_close_internal(dir_store, session, dir_store_fh);
    }
    return (ret);
}

/*
 * dir_store_rename --
 *     POSIX rename, not supported for cloud objects.
 */
static int
dir_store_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    (void)to;    /* unused */
    (void)flags; /* unused */

    return (dir_store_err(
      FS2DS(file_system), session, ENOTSUP, "%s: rename of file not supported", from));
}

/*
 * dir_store_remove --
 *     Calls the corresponding remove method of the file system behind our dir_store storage source.
 */
static int
dir_store_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    DIR_STORE *dir_store;
    int ret;
    char *bucket_path, *cache_path;

    bucket_path = cache_path = NULL;
    dir_store = ((DIR_STORE_FILE_SYSTEM *)file_system)->dir_store;
    ret = 0;

    if (dir_store->cache) {
        /* Check to see if the file exists in the cache directory before attempting to remove it. */
        if ((ret = dir_store_cache_path(file_system, name, &cache_path)) != 0)
            goto err;
        if ((ret = dir_store_remove_if_exists(file_system, session, cache_path, flags)) != 0)
            goto err;
    }

    /* Check to see if the file exists in the bucket directory before attempting to remove it. */
    if ((ret = dir_store_bucket_path(file_system, name, &bucket_path)) != 0)
        goto err;
    if ((ret = dir_store_remove_if_exists(file_system, session, bucket_path, flags)) != 0)
        goto err;

err:
    free(bucket_path);
    if (cache_path != NULL)
        free(cache_path);
    return (ret);
}

/*
 * dir_store_remove_if_exists --
 *     Checks to see if the specified file exists in the directory before attempting to remove it.
 */
static int
dir_store_remove_if_exists(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char *file_path, uint32_t flags)
{
    struct stat sb;
    DIR_STORE_FILE_SYSTEM *dir_store_fs;
    WT_FILE_SYSTEM *wt_fs;
    int ret;

    dir_store_fs = (DIR_STORE_FILE_SYSTEM *)file_system;
    wt_fs = dir_store_fs->wt_fs;

    ret = stat(file_path, &sb);
    if (ret != 0) {
        if (errno != ENOENT) {
            ret = dir_store_err(
              FS2DS(file_system), session, errno, "%s: dir_store_remove stat", file_path);
            goto err;
        }
        ret = 0;
    } else {
        if ((ret = wt_fs->fs_remove(wt_fs, session, file_path, flags)) != 0) {
            ret =
              dir_store_err(FS2DS(file_system), session, errno, "%s: dir_store_remove", file_path);
            goto err;
        }
    }

err:
    return (ret);
}

/*
 * dir_store_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
dir_store_size(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    struct stat sb;
    DIR_STORE *dir_store;
    int ret;

    dir_store = FS2DS(file_system);
    dir_store->op_count++;
    *sizep = 0;

    if ((ret = dir_store_stat(file_system, session, name, "ss_size", true, &sb)) != 0)
        goto err;

    *sizep = sb.st_size;
err:
    return (ret);
}

/*
 * dir_store_terminate --
 *     Discard any resources on termination
 */
static int
dir_store_terminate(WT_STORAGE_SOURCE *storage, WT_SESSION *session)
{
    DIR_STORE_FILE_HANDLE *dir_store_fh, *safe_fh;
    DIR_STORE *dir_store;
    int ret;

    ret = 0;
    dir_store = (DIR_STORE *)storage;

    if (--dir_store->reference_count != 0)
        return (0);

    dir_store->op_count++;

    /*
     * We should be single threaded at this point, so it is safe to destroy the lock and access the
     * file handle list without locking it.
     */
    if ((ret = pthread_rwlock_destroy(&dir_store->file_handle_lock)) != 0)
        (void)dir_store_err(dir_store, session, ret, "terminate: pthread_rwlock_destroy");

    TAILQ_FOREACH_SAFE(dir_store_fh, &dir_store->fileq, q, safe_fh)
    dir_store_file_close_internal(dir_store, session, dir_store_fh);

    free(dir_store);
    return (ret);
}

/*
 * dir_store_file_close --
 *     ANSI C close.
 */
static int
dir_store_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    DIR_STORE *dir_store;
    DIR_STORE_FILE_HANDLE *dir_store_fh;
    int ret, t_ret;

    ret = 0;
    dir_store_fh = (DIR_STORE_FILE_HANDLE *)file_handle;
    dir_store = dir_store_fh->dir_store;

    dir_store->fh_ops++;
    if ((ret = pthread_rwlock_wrlock(&dir_store->file_handle_lock)) != 0)
        /* There really isn't anything more we can do. It will get cleaned up on terminate. */
        return (dir_store_err(dir_store, session, ret, "file handle close: pthread_rwlock_wrlock"));

    TAILQ_REMOVE(&dir_store->fileq, dir_store_fh, q);

    if ((ret = pthread_rwlock_unlock(&dir_store->file_handle_lock)) != 0)
        (void)dir_store_err(dir_store, session, ret, "file handle close: pthread_rwlock_unlock");

    if ((t_ret = dir_store_file_close_internal(dir_store, session, dir_store_fh)) != 0) {
        if (ret == 0)
            ret = t_ret;
    }

    return (ret);
}

/*
 * dir_store_file_close_internal --
 *     Internal file handle close.
 */
static int
dir_store_file_close_internal(
  DIR_STORE *dir_store, WT_SESSION *session, DIR_STORE_FILE_HANDLE *dir_store_fh)
{
    int ret;
    WT_FILE_HANDLE *wt_fh;

    ret = 0;
    wt_fh = dir_store_fh->fh;
    if (wt_fh != NULL && (ret = wt_fh->close(wt_fh, session)) != 0)
        ret = dir_store_err(dir_store, session, ret, "WT_FILE_HANDLE->close: close");

    free(dir_store_fh->iface.name);
    free(dir_store_fh);

    return (ret);
}

/*
 * dir_store_file_lock --
 *     Lock/unlock a file.
 */
static int
dir_store_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    /* Locks are always granted. */

    (void)session; /* Unused */
    (void)lock;    /* Unused */

    ((DIR_STORE_FILE_HANDLE *)file_handle)->dir_store->fh_ops++;
    return (0);
}

/*
 * dir_store_file_read --
 *     POSIX pread.
 */
static int
dir_store_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    DIR_STORE_FILE_HANDLE *dir_store_fh;
    WT_FILE_HANDLE *wt_fh;

    dir_store_fh = (DIR_STORE_FILE_HANDLE *)file_handle;
    wt_fh = dir_store_fh->fh;

    dir_store_fh->dir_store->read_ops++;
    return (wt_fh->fh_read(wt_fh, session, offset, len, buf));
}

/*
 * dir_store_file_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
dir_store_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    DIR_STORE_FILE_HANDLE *dir_store_fh;
    WT_FILE_HANDLE *wt_fh;

    dir_store_fh = (DIR_STORE_FILE_HANDLE *)file_handle;
    wt_fh = dir_store_fh->fh;

    dir_store_fh->dir_store->fh_ops++;
    return (wt_fh->fh_size(wt_fh, session, sizep));
}

/*
 * dir_store_file_sync --
 *     Ensure the content of the dir_store file is stable.
 */
static int
dir_store_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    /* This is a no-op.  We could also disallow it. */
    (void)file_handle;
    (void)session;
    return (0);
}

/*
 * dir_store_file_write --
 *     POSIX pwrite.
 */
static int
dir_store_file_write(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, const void *buf)
{
    DIR_STORE_FILE_HANDLE *dir_store_fh;

    (void)offset;
    (void)len;
    (void)buf;

    dir_store_fh = (DIR_STORE_FILE_HANDLE *)file_handle;
    return (dir_store_err(dir_store_fh->dir_store, session, ENOTSUP,
      "ss_open_object: write not supported: %s", dir_store_fh->iface.name));
}

/*
 * wiredtiger_extension_init --
 *     A simple shared library encryption example.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    DIR_STORE *dir_store;
    int ret;

    if ((dir_store = calloc(1, sizeof(DIR_STORE))) == NULL)
        return (errno);
    dir_store->wt_api = connection->get_extension_api(connection);
    if ((ret = pthread_rwlock_init(&dir_store->file_handle_lock, NULL)) != 0) {
        (void)dir_store_err(dir_store, NULL, ret, "pthread_rwlock_init");
        free(dir_store);
        return (ret);
    }

    /*
     * Allocate a dir_store storage structure, with a WT_STORAGE structure as the first field,
     * allowing us to treat references to either type of structure as a reference to the other type.
     */
    dir_store->storage_source.ss_add_reference = dir_store_add_reference;
    dir_store->storage_source.ss_customize_file_system = dir_store_customize_file_system;
    dir_store->storage_source.ss_flush = dir_store_flush;
    dir_store->storage_source.ss_flush_finish = dir_store_flush_finish;
    dir_store->storage_source.terminate = dir_store_terminate;

    /*
     * The first reference is implied by the call to add_storage_source.
     */
    dir_store->reference_count = 1;

    /* Cache files locally by default */
    dir_store->cache = 1;

    if ((ret = dir_store_configure(dir_store, config)) != 0) {
        free(dir_store);
        return (ret);
    }

    /* Load the storage */
    if ((ret = connection->add_storage_source(
           connection, "dir_store", &dir_store->storage_source, NULL)) != 0) {
        (void)dir_store_err(dir_store, NULL, ret, "WT_CONNECTION->add_storage_source");
        free(dir_store);
    }
    return (ret);
}
