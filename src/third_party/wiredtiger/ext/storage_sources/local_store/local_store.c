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
 * as local files.
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

/* Local storage source structure. */
typedef struct {
    WT_STORAGE_SOURCE storage_source; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */

    /*
     * Locks are used to protect the file handle queue and flush queue.
     */
    pthread_rwlock_t file_handle_lock;
    pthread_rwlock_t flush_lock;

    /*
     * Configuration values are set at startup.
     */
    uint32_t delay_ms;    /* Average length of delay when simulated */
    uint32_t force_delay; /* Force a simulated network delay every N operations */
    uint32_t force_error; /* Force a simulated network error every N operations */
    uint32_t verbose;     /* Verbose level */

    /*
     * Statistics are collected but not yet exposed.
     */
    uint64_t fh_ops;         /* Non-read/write operations in file handles */
    uint64_t object_flushes; /* (What would be) writes to the cloud */
    uint64_t op_count;       /* Number of operations done on local */
    uint64_t read_ops;
    uint64_t write_ops;

    /* Queue of file handles */
    TAILQ_HEAD(local_file_handle_qh, local_file_handle) fileq;
    TAILQ_HEAD(local_flush_qh, local_flush_item) flushq;

} LOCAL_STORAGE;

typedef struct {
    /* Must come first - this is the interface for the file system we are implementing. */
    WT_FILE_SYSTEM file_system;
    LOCAL_STORAGE *local_storage;

    /* This is WiredTiger's file system, it is used in implementing the local file system. */
    WT_FILE_SYSTEM *wt_fs;

    char *auth_token; /* Identifier for key management system */
    char *bucket_dir; /* Directory that stands in for cloud storage bucket */
    char *cache_dir;  /* Directory for pre-flushed objects and cached objects */
    char *fs_prefix;  /* File system prefix, allowing for a "directory" within a bucket */
} LOCAL_FILE_SYSTEM;

/*
 * Indicates a object that has not yet been flushed.
 */
typedef struct local_flush_item {
    char *src_path; /* File name to copy from, object name derived from this */

    /*
     * These fields would be used in performing a flush.
     */
    char *auth_token;               /* Identifier for key management system */
    char *bucket;                   /* Bucket name */
    WT_FS_OPEN_FILE_TYPE file_type; /* File type */

    TAILQ_ENTRY(local_flush_item) q; /* Queue of items */
} LOCAL_FLUSH_ITEM;

typedef struct local_file_handle {
    WT_FILE_HANDLE iface; /* Must come first */

    LOCAL_STORAGE *local;    /* Enclosing storage source */
    WT_FILE_HANDLE *fh;      /* File handle */
    char *path;              /* Path name of file */
    LOCAL_FLUSH_ITEM *flush; /* Flush information, set if newly created */

    TAILQ_ENTRY(local_file_handle) q; /* Queue of handles */
} LOCAL_FILE_HANDLE;

/*
 * Forward function declarations for internal functions
 */
static int local_configure(LOCAL_STORAGE *, WT_CONFIG_ARG *);
static int local_configure_int(LOCAL_STORAGE *, WT_CONFIG_ARG *, const char *, uint32_t *);
static int local_delay(LOCAL_STORAGE *);
static int local_err(LOCAL_STORAGE *, WT_SESSION *, int, const char *, ...);
static void local_flush_free(LOCAL_FLUSH_ITEM *);
static int local_get_directory(const char *, ssize_t len, char **);
static int local_location_path(WT_FILE_SYSTEM *, const char *, char **);
static int local_writeable(LOCAL_STORAGE *, WT_SESSION *, const char *, bool *);

/*
 * Forward function declarations for storage source API implementation
 */
static int local_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int local_customize_file_system(WT_STORAGE_SOURCE *, WT_SESSION *, const char *,
  const char *, const char *, const char *, WT_FILE_SYSTEM **);
static int local_flush(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *, const char *);
static int local_flush_one(LOCAL_STORAGE *, WT_SESSION *, LOCAL_FLUSH_ITEM *);
static int local_terminate(WT_STORAGE_SOURCE *, WT_SESSION *);

/*
 * Forward function declarations for file system API implementation
 */
static int local_directory_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int local_directory_list_internal(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t, char ***, uint32_t *);
static int local_directory_list_single(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int local_directory_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int local_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);
static int local_open(WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE file_type,
  uint32_t, WT_FILE_HANDLE **);
static int local_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int local_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int local_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);

/*
 * Forward function declarations for file handle API implementation
 */
static int local_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int local_file_close_internal(LOCAL_STORAGE *, WT_SESSION *, LOCAL_FILE_HANDLE *);
static int local_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int local_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int local_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int local_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int local_file_write(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);

/*
 * Report an error for a file operation. Note that local_err returns its third argument, and this
 * macro will too.
 */
#define FS2LOCAL(fs) (((LOCAL_FILE_SYSTEM *)(fs))->local_storage)

#define VERBOSE(local, ...)               \
    do {                                  \
        if ((local)->verbose > 0)         \
            fprintf(stderr, __VA_ARGS__); \
    } while (0);
#define SHOW_STRING(s) (((s) == NULL) ? "<null>" : (s))

/*
 * local_configure
 *     Parse the configuration for the keys we care about.
 */
static int
local_configure(LOCAL_STORAGE *local, WT_CONFIG_ARG *config)
{
    int ret;

    if ((ret = local_configure_int(local, config, "delay_ms", &local->delay_ms)) != 0)
        return (ret);
    if ((ret = local_configure_int(local, config, "force_delay", &local->force_delay)) != 0)
        return (ret);
    if ((ret = local_configure_int(local, config, "force_error", &local->force_error)) != 0)
        return (ret);
    if ((ret = local_configure_int(local, config, "verbose", &local->verbose)) != 0)
        return (ret);

    return (0);
}

/*
 * local_configure_int
 *     Look for a particular configuration key, and return its integer value.
 */
static int
local_configure_int(LOCAL_STORAGE *local, WT_CONFIG_ARG *config, const char *key, uint32_t *valuep)
{
    WT_CONFIG_ITEM v;
    int ret;

    ret = 0;

    if ((ret = local->wt_api->config_get(local->wt_api, NULL, config, key, &v)) == 0) {
        if (v.len == 0 || v.type != WT_CONFIG_ITEM_NUM)
            ret = local_err(local, NULL, EINVAL, "force_error config arg: integer required");
        else
            *valuep = (uint32_t)v.val;
    } else if (ret == WT_NOTFOUND)
        ret = 0;
    else
        ret = local_err(local, NULL, EINVAL, "WT_API->config_get");

    return (ret);
}

/*
 * local_delay --
 *     Add any artificial delay or simulated network error during an object transfer.
 */
static int
local_delay(LOCAL_STORAGE *local)
{
    struct timeval tv;
    int ret;

    ret = 0;
    if (local->force_delay != 0 && local->object_flushes % local->force_delay == 0) {
        VERBOSE(local,
          "Artificial delay %" PRIu32 " milliseconds after %" PRIu64 " object flushes\n",
          local->delay_ms, local->object_flushes);
        tv.tv_sec = local->delay_ms / 1000;
        tv.tv_usec = (local->delay_ms % 1000) * 1000;
        (void)select(0, NULL, NULL, NULL, &tv);
    }
    if (local->force_error != 0 && local->object_flushes % local->force_error == 0) {
        VERBOSE(local, "Artificial error returned after %" PRIu64 " object flushes\n",
          local->object_flushes);
        ret = ENETUNREACH;
    }

    return (ret);
}

/*
 * local_err --
 *     Print errors from the interface. Returns "ret", the third argument.
 */
static int
local_err(LOCAL_STORAGE *local, WT_SESSION *session, int ret, const char *format, ...)
{
    va_list ap;
    WT_EXTENSION_API *wt_api;
    char buf[1000];

    va_start(ap, format);
    wt_api = local->wt_api;
    if (vsnprintf(buf, sizeof(buf), format, ap) > (int)sizeof(buf))
        wt_api->err_printf(wt_api, session, "local_storage: error overflow");
    wt_api->err_printf(
      wt_api, session, "local_storage: %s: %s", wt_api->strerror(wt_api, session, ret), buf);
    va_end(ap);

    return (ret);
}

/*
 * local_flush_free --
 *     Free storage for a flush item.
 */
static void
local_flush_free(LOCAL_FLUSH_ITEM *flush)
{
    if (flush != NULL) {
        free(flush->auth_token);
        free(flush->bucket);
        free(flush->src_path);
        free(flush);
    }
}

/*
 * local_get_directory --
 *     Return a copy of a directory name after verifying that it is a directory.
 */
static int
local_get_directory(const char *s, ssize_t len, char **copy)
{
    struct stat sb;
    int ret;
    char *dirname;

    if (len == -1)
        len = (ssize_t)strlen(s);
    dirname = strndup(s, (size_t)len + 1); /* Room for null */
    if (dirname == NULL)
        return (ENOMEM);
    ret = stat(dirname, &sb);
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
 * local_writeable --
 *     Check if a file is local and writeable.
 */
static int
local_writeable(LOCAL_STORAGE *local, WT_SESSION *session, const char *name, bool *writeablep)
{
    struct stat sb;
    int ret;

    *writeablep = false;
    ret = stat(name, &sb);
    if (ret == 0) {
        /*
         * Check the write bits. If the file is not writeable, it has been flushed.
         */
        *writeablep = ((sb.st_mode & 0222) != 0);
    } else if (errno == ENOENT) {
        /*
         * Does not exist locally. It could be in the cloud, at any rate it is not writeable, but
         * not an error.
         */
        ret = 0;
    } else
        ret = local_err(local, session, errno, "%s: stat", name);

    return (ret);
}

/*
 * local_location_path --
 *     Construct a pathname from the file system and local name.
 */
int
local_location_path(WT_FILE_SYSTEM *file_system, const char *name, char **pathp)
{
    LOCAL_FILE_SYSTEM *local_fs;
    size_t len;
    int ret;
    char *p;

    ret = 0;
    local_fs = (LOCAL_FILE_SYSTEM *)file_system;

    len = strlen(local_fs->cache_dir) + strlen(local_fs->fs_prefix) + strlen(name) + 2;
    if ((p = malloc(len)) == NULL)
        return (local_err(FS2LOCAL(file_system), NULL, ENOMEM, "local_location_path"));
    snprintf(p, len, "%s/%s%s", local_fs->cache_dir, local_fs->fs_prefix, name);
    *pathp = p;
    return (ret);
}

/*
 * local_customize_file_system --
 *     Return a customized file system to access the local storage source objects.
 */
static int
local_customize_file_system(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *bucket_name, const char *prefix, const char *auth_token, const char *config,
  WT_FILE_SYSTEM **file_systemp)
{
    LOCAL_STORAGE *local;
    LOCAL_FILE_SYSTEM *fs;
    WT_CONFIG_ITEM cachedir;
    WT_FILE_SYSTEM *wt_fs;
    int ret;

    local = (LOCAL_STORAGE *)storage_source;

    fs = NULL;
    ret = 0;

    /* Parse configuration string. */
    if ((ret = local->wt_api->config_get_string(
           local->wt_api, session, config, "cache_directory", &cachedir)) != 0) {
        if (ret == WT_NOTFOUND) {
            ret = 0;
            cachedir.len = 0;
        } else {
            ret = local_err(local, session, ret, "customize_file_system: config parsing");
            goto err;
        }
    }
    /* Default is "." directory. */
    if (cachedir.len == 0) {
        cachedir.str = ".";
        cachedir.len = 1;
    }

    if ((ret = local->wt_api->file_system_get(local->wt_api, session, &wt_fs)) != 0) {
        ret =
          local_err(local, session, ret, "local_file_system: cannot get WiredTiger file system");
        goto err;
    }
    if ((fs = calloc(1, sizeof(LOCAL_FILE_SYSTEM))) == NULL) {
        ret = local_err(local, session, ENOMEM, "local_file_system");
        goto err;
    }
    fs->local_storage = local;
    fs->wt_fs = wt_fs;

    if ((fs->auth_token = strdup(auth_token)) == NULL) {
        ret = local_err(local, session, ENOMEM, "local_file_system.auth_token");
        goto err;
    }
    /*
     * Get the bucket directory and the cache directory.
     */
    if ((ret = local_get_directory(bucket_name, -1, &fs->bucket_dir)) != 0) {
        ret = local_err(local, session, ret, "%s: bucket directory", bucket_name);
        goto err;
    }
    if ((ret = local_get_directory(cachedir.str, (ssize_t)cachedir.len, &fs->cache_dir)) != 0) {
        ret =
          local_err(local, session, ret, "%*s: cache directory", (int)cachedir.len, cachedir.str);
        goto err;
    }
    if ((fs->fs_prefix = strdup(prefix)) == NULL) {
        ret = local_err(local, session, ENOMEM, "local_file_system.prefix");
        goto err;
    }
    fs->file_system.fs_directory_list = local_directory_list;
    fs->file_system.fs_directory_list_single = local_directory_list_single;
    fs->file_system.fs_directory_list_free = local_directory_list_free;
    fs->file_system.fs_exist = local_exist;
    fs->file_system.fs_open_file = local_open;
    fs->file_system.fs_remove = local_remove;
    fs->file_system.fs_rename = local_rename;
    fs->file_system.fs_size = local_size;
    fs->file_system.terminate = local_fs_terminate;

err:
    if (ret == 0)
        *file_systemp = &fs->file_system;
    else if (fs != NULL) {
        free(fs->auth_token);
        free(fs->bucket_dir);
        free(fs->cache_dir);
        free(fs->fs_prefix);
        free(fs);
    }
    return (ret);
}

/*
 * local_exist --
 *     Return if the file exists.
 */
static int
local_exist(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    struct stat sb;
    LOCAL_STORAGE *local;
    int ret;
    char *path;

    local = FS2LOCAL(file_system);
    path = NULL;

    local->op_count++;
    if ((ret = local_location_path(file_system, name, &path)) != 0)
        goto err;

    ret = stat(path, &sb);
    if (ret == 0)
        *existp = true;
    else if (errno == ENOENT) {
        ret = 0;
        *existp = false;
    } else
        ret = local_err(local, session, errno, "%s: ss_exist stat", path);

err:
    free(path);
    return (ret);
}

/*
 * local_flush --
 *     Return when the files have been flushed.
 */
static int
local_flush(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
  const char *name, const char *config)
{
    LOCAL_STORAGE *local;
    LOCAL_FLUSH_ITEM *flush, *safe_flush;
    int ret, t_ret;
    char *match;

    (void)config; /* Unused */

    /*
     * This implementation does not do anything meaningful on flush. However, we do track which
     * objects have not yet been flushed and note which ones need to be flushed now.
     */
    ret = 0;
    local = (LOCAL_STORAGE *)storage_source;
    match = NULL;

    if (file_system == NULL && name != NULL)
        return local_err(local, session, EINVAL, "flush: cannot specify name without file system");

    local->op_count++;
    if (file_system != NULL) {
        if ((ret = local_location_path(file_system, name == NULL ? "" : name, &match)) != 0)
            goto err;
    }
    VERBOSE(local, "Flush: match=%s\n", SHOW_STRING(match));

    /*
     * Note: we retain the lock on the data structure while flushing all entries. This is fine for
     * our local file implementation, when we don't have to do anything to flush, but for a cloud
     * implementation, we'll want some way to not hold the lock while transferring data.
     */
    if ((ret = pthread_rwlock_wrlock(&local->flush_lock)) != 0) {
        (void)local_err(local, session, ret, "flush: pthread_rwlock_wrlock");
        goto err;
    }

    TAILQ_FOREACH_SAFE(flush, &local->flushq, q, safe_flush)
    {
        if (match != NULL) {
            /*
             * We must match against the bucket and the name if given.
             * Our match string is of the form:
             *   <bucket_name>/<fs_prefix><name>
             *
             * If name is given, we must match the entire path.
             * If name is not given, we must match up to the beginning
             * of the name.
             */
            if (name != NULL) {
                /* Exact name match required. */
                if (strcmp(flush->src_path, match) != 0)
                    continue;
            }
            /* No name specified, everything up to the name must match. */
            else if (strncmp(flush->src_path, match, strlen(match)) != 0)
                continue;
        }
        if ((t_ret = local_flush_one(local, session, flush)) != 0 && ret == 0)
            ret = t_ret;
        TAILQ_REMOVE(&local->flushq, flush, q);
        local_flush_free(flush);
    }

    if ((t_ret = pthread_rwlock_unlock(&local->flush_lock)) != 0) {
        (void)local_err(local, session, t_ret, "flush: pthread_rwlock_unlock");
        if (ret == 0)
            ret = t_ret;
    }

err:
    free(match);

    return (ret);
}

/*
 * local_flush_one --
 *     Flush one item on the flush queue.
 */
static int
local_flush_one(LOCAL_STORAGE *local, WT_SESSION *session, LOCAL_FLUSH_ITEM *flush)
{
    WT_FILE_HANDLE *dest, *src;
    WT_FILE_SYSTEM *wt_fs;
    wt_off_t copy_size, file_size, left;
    int ret, t_ret;
    char *object_name;
    char buffer[1024 * 64];
    char dest_path[1024];
    ssize_t pos;

    ret = 0;
    src = dest = NULL;

    object_name = strrchr(flush->src_path, '/');
    if (object_name == NULL) {
        ret = local_err(local, session, errno, "%s: unexpected src path", flush->src_path);
        goto err;
    }
    object_name++;

    /*
     * Here's where we flush the file to the cloud. This "local" implementation copies the file to
     * the bucket directory.
     */
    VERBOSE(local, "Flush object: from=%s, bucket=%s, object=%s, auth_token=%s, \n",
      flush->src_path, flush->bucket, object_name, flush->auth_token);

    if ((ret = local_delay(local)) != 0)
        goto err;

    if ((ret = local->wt_api->file_system_get(local->wt_api, session, &wt_fs)) != 0) {
        ret =
          local_err(local, session, ret, "local_file_system: cannot get WiredTiger file system");
        goto err;
    }
    snprintf(dest_path, sizeof(dest_path), "%s/%s", flush->bucket, object_name);

    if ((ret = wt_fs->fs_open_file(
           wt_fs, session, flush->src_path, flush->file_type, WT_FS_OPEN_READONLY, &src)) != 0) {
        ret = local_err(local, session, ret, "%s: cannot open for read", flush->src_path);
        goto err;
    }

    if ((ret = wt_fs->fs_open_file(
           wt_fs, session, dest_path, flush->file_type, WT_FS_OPEN_CREATE, &dest)) != 0) {
        ret = local_err(local, session, ret, "%s: cannot create", dest_path);
        goto err;
    }
    if ((ret = wt_fs->fs_size(wt_fs, session, flush->src_path, &file_size)) != 0) {
        ret = local_err(local, session, ret, "%s: cannot get size", flush->src_path);
        goto err;
    }
    for (pos = 0, left = file_size; left > 0; pos += copy_size, left -= copy_size) {
        copy_size = left < (wt_off_t)sizeof(buffer) ? left : (wt_off_t)sizeof(buffer);
        if ((ret = src->fh_read(src, session, pos, (size_t)copy_size, buffer)) != 0) {
            ret = local_err(local, session, ret, "%s: cannot read", flush->src_path);
            goto err;
        }
        if ((ret = dest->fh_write(dest, session, pos, (size_t)copy_size, buffer)) != 0) {
            ret = local_err(local, session, ret, "%s: cannot write", dest_path);
            goto err;
        }
    }
    if ((ret = dest->fh_sync(dest, session)) != 0) {
        ret = local_err(local, session, ret, "%s: cannot sync", dest_path);
        goto err;
    }
    local->object_flushes++;

err:
    /* When we're done with flushing this file, set the file to readonly. */
    if (ret == 0 && (ret = chmod(flush->src_path, 0444)) < 0)
        ret = local_err(local, session, errno, "%s: chmod flushed file failed", flush->src_path);
    if (src != NULL && (t_ret = src->close(src, session)) != 0)
        if (ret == 0)
            ret = t_ret;
    if (dest != NULL && (t_ret = dest->close(dest, session)) != 0)
        if (ret == 0)
            ret = t_ret;

    return (ret);
}

/*
 * local_directory_list --
 *     Return a list of object names for the given location.
 */
static int
local_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    FS2LOCAL(file_system)->op_count++;
    return (
      local_directory_list_internal(file_system, session, directory, prefix, 0, dirlistp, countp));
}

/*
 * local_directory_list_single --
 *     Return a single file name for the given location.
 */
static int
local_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    FS2LOCAL(file_system)->op_count++;
    return (
      local_directory_list_internal(file_system, session, directory, prefix, 1, dirlistp, countp));
}

/*
 * local_location_list_free --
 *     Free memory allocated by local_location_list.
 */
static int
local_directory_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    (void)session;

    FS2LOCAL(file_system)->op_count++;
    if (dirlist != NULL) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }
    return (0);
}

/*
 * local_location_list_internal --
 *     Return a list of object names for the given location.
 */
static int
local_directory_list_internal(WT_FILE_SYSTEM *file_system, WT_SESSION *session,
  const char *directory, const char *prefix, uint32_t limit, char ***dirlistp, uint32_t *countp)
{
    struct dirent *dp;
    DIR *dirp;
    LOCAL_FILE_SYSTEM *local_fs;
    LOCAL_STORAGE *local;
    size_t alloc_sz, fs_prefix_len, dir_len, prefix_len;
    uint32_t allocated, count;
    int ret, t_ret;
    char **entries, **new_entries;
    const char *basename;

    local_fs = (LOCAL_FILE_SYSTEM *)file_system;
    local = local_fs->local_storage;
    entries = NULL;
    allocated = count = 0;
    fs_prefix_len = strlen(local_fs->fs_prefix);
    dir_len = (directory == NULL ? 0 : strlen(directory));
    prefix_len = (prefix == NULL ? 0 : strlen(prefix));
    ret = 0;

    *dirlistp = NULL;
    *countp = 0;

    if ((dirp = opendir(local_fs->cache_dir)) == NULL) {
        ret = errno;
        if (ret == 0)
            ret = EINVAL;
        return (
          local_err(local, session, ret, "%s: ss_directory_list: opendir", local_fs->cache_dir));
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

        /* Skip files not associated with our file system prefix. */
        if (strncmp(basename, local_fs->fs_prefix, fs_prefix_len) != 0)
            continue;

        basename += fs_prefix_len;
        /* The list of files is optionally filtered by a prefix. */
        if (prefix != NULL && strncmp(basename, prefix, prefix_len) != 0)
            continue;

        if (count >= allocated) {
            allocated += 10;
            alloc_sz = sizeof(char *) * allocated;
            if ((new_entries = realloc(entries, alloc_sz)) == NULL) {
                ret = ENOMEM;
                goto err;
            }
            entries = new_entries;
        }
        if ((entries[count] = strdup(basename)) == NULL) {
            ret = ENOMEM;
            goto err;
        }
        count++;
    }

    *dirlistp = entries;
    *countp = count;

err:
    if (closedir(dirp) != 0) {
        t_ret =
          local_err(local, session, errno, "%s: ss_directory_list: closedir", local_fs->cache_dir);
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
 * local_fs_terminate --
 *     Discard any resources on termination of the file system
 */
static int
local_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    LOCAL_FILE_SYSTEM *local_fs;

    (void)session; /* unused */

    local_fs = (LOCAL_FILE_SYSTEM *)file_system;
    FS2LOCAL(file_system)->op_count++;
    free(local_fs->auth_token);
    free(local_fs->bucket_dir);
    free(local_fs->cache_dir);
    free(local_fs->fs_prefix);
    free(file_system);

    return (0);
}

/*
 * local_open --
 *     fopen for our local storage source
 */
static int
local_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    LOCAL_FILE_HANDLE *local_fh;
    LOCAL_FILE_SYSTEM *local_fs;
    LOCAL_FLUSH_ITEM *flush;
    LOCAL_STORAGE *local;
    WT_FILE_HANDLE *file_handle, *wt_fh;
    WT_FILE_SYSTEM *wt_fs;
    struct stat sb;
    int ret;
    bool exists;

    (void)flags; /* Unused */

    ret = 0;
    *file_handlep = NULL;
    local_fh = NULL;
    local_fs = (LOCAL_FILE_SYSTEM *)file_system;
    local = local_fs->local_storage;
    wt_fs = local_fs->wt_fs;

    /*
     * We expect that the local file system will be used narrowly, like when creating or opening a
     * data file or turtle file. It would be unexpected to try to open a non-data file (like a log
     * file) in that narrow part of code, so we make it an error here.
     *
     * Relaxing this constraint to allow opening of, say, log files, would be straightforward - we
     * would not translate the path or do any tracking for flushing. But there's a catch. Other
     * parts of the API, like remove and rename, have no flag indicating that they are operating on
     * a log file, so we wouldn't know whether to do path translation. Of course, we could peek at
     * the name, but that would be bad form.
     */
    if (file_type != WT_FS_OPEN_FILE_TYPE_DATA && file_type != WT_FS_OPEN_FILE_TYPE_REGULAR)
        return (local_err(
          local, session, EINVAL, "%s: open: only data file and regular types supported", name));

    /* Create a new handle. */
    if ((local_fh = calloc(1, sizeof(LOCAL_FILE_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }
    if ((ret = local_location_path(file_system, name, &local_fh->path)) != 0)
        goto err;

    ret = stat(local_fh->path, &sb);
    if (ret != 0 && errno != ENOENT)
        ret = local_err(local, session, errno, "%s: local_open stat", local_fh->path);
    exists = (ret == 0);

    /*
     * TODO: tiered: If the file doesn't exist locally, make a copy of it from the cloud here.
     *
     */
#if 0
    if ((flags & WT_FS_OPEN_READONLY) != 0 && !exists) {
    }
#endif

    if ((flags & WT_FS_OPEN_CREATE) != 0 && !exists) {
        if ((flush = calloc(1, sizeof(LOCAL_FLUSH_ITEM))) == NULL) {
            ret = ENOMEM;
            goto err;
        }
        local_fh->flush = flush;

        if ((flush->auth_token = strdup(local_fs->auth_token)) == NULL) {
            ret = local_err(local, session, ENOMEM, "open.auth_token");
            goto err;
        }
        if ((flush->bucket = strdup(local_fs->bucket_dir)) == NULL) {
            ret = local_err(local, session, ENOMEM, "open.bucket");
            goto err;
        }
        flush->file_type = file_type;
    }

    if ((wt_fs->fs_open_file(wt_fs, session, local_fh->path, file_type, flags, &wt_fh)) < 0) {
        ret = local_err(local, session, errno, "ss_open_object: open: %s", local_fh->path);
        goto err;
    }
    local_fh->fh = wt_fh;
    local_fh->local = local;

    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)local_fh;

    /*
     * Setup the function call table for our custom storage source. Set the function pointer to NULL
     * where our implementation doesn't support the functionality.
     */
    file_handle->close = local_file_close;
    file_handle->fh_advise = NULL;
    file_handle->fh_extend = NULL;
    file_handle->fh_extend_nolock = NULL;
    file_handle->fh_lock = local_file_lock;
    file_handle->fh_map = NULL;
    file_handle->fh_map_discard = NULL;
    file_handle->fh_map_preload = NULL;
    file_handle->fh_unmap = NULL;
    file_handle->fh_read = local_file_read;
    file_handle->fh_size = local_file_size;
    file_handle->fh_sync = local_file_sync;
    file_handle->fh_sync_nowait = NULL;
    file_handle->fh_truncate = NULL;
    file_handle->fh_write = local_file_write;
    if ((file_handle->name = strdup(name)) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    if ((ret = pthread_rwlock_wrlock(&local->file_handle_lock)) != 0) {
        (void)local_err(local, session, ret, "ss_open_object: pthread_rwlock_wrlock");
        goto err;
    }
    TAILQ_INSERT_HEAD(&local->fileq, local_fh, q);
    if ((ret = pthread_rwlock_unlock(&local->file_handle_lock)) != 0) {
        (void)local_err(local, session, ret, "ss_open_object: pthread_rwlock_unlock");
        goto err;
    }

    *file_handlep = file_handle;

    VERBOSE(
      local, "File opened: %s final path=%s\n", SHOW_STRING(name), SHOW_STRING(local_fh->path));

err:
    if (ret != 0) {
        if (local_fh != NULL)
            local_file_close_internal(local, session, local_fh);
    }
    return (ret);
}

/*
 * local_rename --
 *     POSIX rename, for files not yet flushed to the cloud. If a file has been flushed, we don't
 *     support this operation. That is because cloud implementations may not support it, and more
 *     importantly, we consider anything in the cloud to be readonly as far as the custom file
 *     system is concerned.
 */
static int
local_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    LOCAL_FILE_SYSTEM *local_fs;
    LOCAL_FLUSH_ITEM *flush;
    LOCAL_STORAGE *local;
    WT_FILE_SYSTEM *wt_fs;
    int ret, t_ret;
    char *copy, *from_path, *to_path;
    bool writeable;

    local = FS2LOCAL(file_system);
    local_fs = (LOCAL_FILE_SYSTEM *)file_system;
    wt_fs = local_fs->wt_fs;
    from_path = to_path = NULL;
    writeable = false;

    local->op_count++;
    if ((ret = local_location_path(file_system, from, &from_path)) != 0)
        goto err;
    if ((ret = local_writeable(local, session, from_path, &writeable)) != 0)
        goto err;
    if (!writeable) {
        /* If not writeable, we assume it is flushed and rename is not allowed. */
        ret = local_err(local, session, ENOTSUP, "%s: rename of flushed file not allowed", from);
        goto err;
    }

    if ((ret = local_location_path(file_system, to, &to_path)) != 0)
        goto err;

    if ((ret = wt_fs->fs_rename(wt_fs, session, from_path, to_path, flags)) != 0) {
        ret = local_err(local, session, ret, "fs_rename");
        goto err;
    }

    /*
     * Find any flush entry that matches, and rename that too.
     */
    if ((ret = pthread_rwlock_wrlock(&local->flush_lock)) != 0) {
        ret = local_err(local, session, ret, "ss_remove: pthread_rwlock_wrlock");
        goto err;
    }

    TAILQ_FOREACH (flush, &local->flushq, q) {
        if (strcmp(flush->src_path, from_path) == 0) {
            if ((copy = strdup(to_path)) == NULL)
                ret = ENOMEM;
            else {
                free(flush->src_path);
                flush->src_path = copy;
            }
            break;
        }
    }

    if ((t_ret = pthread_rwlock_unlock(&local->flush_lock)) != 0) {
        (void)local_err(local, session, t_ret, "ss_remove: pthread_rwlock_unlock");
        if (ret == 0)
            ret = t_ret;
        goto err;
    }

err:
    free(from_path);
    free(to_path);
    return (ret);
}

/*
 * local_remove --
 *     POSIX remove, for files not yet flushed to the cloud. If a file has been flushed, we don't
 *     support this operation. We consider anything in the cloud to be readonly as far as the custom
 *     file system is concerned.
 */
static int
local_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    LOCAL_FLUSH_ITEM *flush;
    LOCAL_STORAGE *local;
    int ret;
    char *path;
    bool writeable;

    (void)flags; /* Unused */

    local = FS2LOCAL(file_system);
    path = NULL;
    writeable = false;

    local->op_count++;
    if ((ret = local_location_path(file_system, name, &path)) != 0)
        goto err;
    if ((ret = local_writeable(local, session, path, &writeable)) != 0)
        goto err;
    if (!writeable) {
        /* If not writeable, we assume it is flushed and remove is not allowed. */
        ret = local_err(local, session, ENOTSUP, "%s: remove of flushed file not allowed", name);
        goto err;
    }

    ret = unlink(path);
    if (ret != 0) {
        ret = local_err(local, session, errno, "%s: ss_remove unlink", path);
        goto err;
    }

    /*
     * Find any flush entry that matches, and remove that too.
     */
    if ((ret = pthread_rwlock_wrlock(&local->flush_lock)) != 0) {
        ret = local_err(local, session, ret, "ss_remove: pthread_rwlock_wrlock");
        goto err;
    }

    TAILQ_FOREACH (flush, &local->flushq, q) {
        if (strcmp(flush->src_path, path) == 0) {
            TAILQ_REMOVE(&local->flushq, flush, q);
            local_flush_free(flush);
            break;
        }
    }

    if ((ret = pthread_rwlock_unlock(&local->flush_lock)) != 0) {
        ret = local_err(local, session, ret, "ss_remove: pthread_rwlock_unlock");
        goto err;
    }

err:
    free(path);
    return (ret);
}

/*
 * local_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
local_size(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    struct stat sb;
    LOCAL_STORAGE *local;
    int ret;
    char *path;

    local = FS2LOCAL(file_system);
    path = NULL;

    local->op_count++;
    if ((ret = local_location_path(file_system, name, &path)) != 0)
        goto err;

    ret = stat(path, &sb);
    if (ret == 0)
        *sizep = sb.st_size;
    else
        ret = local_err(local, session, errno, "%s: ss_size stat", path);

err:
    free(path);
    return (ret);
}

/*
 * local_terminate --
 *     Discard any resources on termination
 */
static int
local_terminate(WT_STORAGE_SOURCE *storage, WT_SESSION *session)
{
    LOCAL_FILE_HANDLE *local_fh, *safe_fh;
    LOCAL_STORAGE *local;
    int ret;

    ret = 0;
    local = (LOCAL_STORAGE *)storage;

    local->op_count++;

    /*
     * We should be single threaded at this point, so it is safe to destroy the lock and access the
     * file handle list without locking it.
     */
    if ((ret = pthread_rwlock_destroy(&local->file_handle_lock)) != 0)
        (void)local_err(local, session, ret, "terminate: pthread_rwlock_destroy");

    TAILQ_FOREACH_SAFE(local_fh, &local->fileq, q, safe_fh)
    local_file_close_internal(local, session, local_fh);

    free(local);
    return (ret);
}

/*
 * local_file_close --
 *     ANSI C close.
 */
static int
local_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    LOCAL_STORAGE *local;
    LOCAL_FILE_HANDLE *local_fh;
    LOCAL_FLUSH_ITEM *flush;
    int ret, t_ret;

    ret = 0;
    local_fh = (LOCAL_FILE_HANDLE *)file_handle;
    local = local_fh->local;

    local->fh_ops++;
    if ((ret = pthread_rwlock_wrlock(&local->file_handle_lock)) != 0)
        /* There really isn't anything more we can do. It will get cleaned up on terminate. */
        return (local_err(local, session, ret, "file handle close: pthread_rwlock_wrlock"));

    TAILQ_REMOVE(&local->fileq, local_fh, q);

    if ((ret = pthread_rwlock_unlock(&local->file_handle_lock)) != 0)
        (void)local_err(local, session, ret, "file handle close: pthread_rwlock_unlock");

    /*
     * If we need to track flushes for this file, save the flush item on our queue.
     */
    if (ret == 0 && ((flush = local_fh->flush)) != NULL) {
        if ((ret = pthread_rwlock_wrlock(&local->flush_lock)) != 0)
            (void)local_err(local, session, ret, "file handle close: pthread_rwlock_wrlock2");

        if (ret == 0) {
            /*
             * Move the flush object from the file handle and to the flush queue. It is now owned by
             * the flush queue and will be freed when that item is flushed.
             */
            TAILQ_INSERT_HEAD(&local->flushq, flush, q);
            local_fh->flush = NULL;

            if ((ret = pthread_rwlock_unlock(&local->flush_lock)) != 0)
                (void)local_err(local, session, ret, "file handle close: pthread_rwlock_unlock2");
            if (ret == 0 && ((flush->src_path = strdup(local_fh->path)) == NULL))
                ret = ENOMEM;
        }
    }

    if ((t_ret = local_file_close_internal(local, session, local_fh)) != 0) {
        if (ret == 0)
            ret = t_ret;
    }

    return (ret);
}

/*
 * local_file_close_internal --
 *     Internal file handle close.
 */
static int
local_file_close_internal(LOCAL_STORAGE *local, WT_SESSION *session, LOCAL_FILE_HANDLE *local_fh)
{
    int ret;
    WT_FILE_HANDLE *wt_fh;

    ret = 0;
    wt_fh = local_fh->fh;
    if ((ret = wt_fh->close(wt_fh, session)) != 0)
        ret = local_err(local, session, ret, "WT_FILE_HANDLE->close: close");

    local_flush_free(local_fh->flush);
    free(local_fh->path);
    free(local_fh->iface.name);
    free(local_fh);

    return (ret);
}

/*
 * local_file_lock --
 *     Lock/unlock a file.
 */
static int
local_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    /* Locks are always granted. */

    (void)session; /* Unused */
    (void)lock;    /* Unused */

    ((LOCAL_FILE_HANDLE *)file_handle)->local->fh_ops++;
    return (0);
}

/*
 * local_file_read --
 *     POSIX pread.
 */
static int
local_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    LOCAL_FILE_HANDLE *local_fh;
    WT_FILE_HANDLE *wt_fh;

    local_fh = (LOCAL_FILE_HANDLE *)file_handle;
    wt_fh = local_fh->fh;

    local_fh->local->read_ops++;
    return (wt_fh->fh_read(wt_fh, session, offset, len, buf));
}

/*
 * local_file_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
local_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    LOCAL_FILE_HANDLE *local_fh;
    WT_FILE_HANDLE *wt_fh;

    local_fh = (LOCAL_FILE_HANDLE *)file_handle;
    wt_fh = local_fh->fh;

    local_fh->local->fh_ops++;
    return (wt_fh->fh_size(wt_fh, session, sizep));
}

/*
 * local_file_sync --
 *     Ensure the content of the local file is stable.
 */
static int
local_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    LOCAL_FILE_HANDLE *local_fh;
    WT_FILE_HANDLE *wt_fh;

    local_fh = (LOCAL_FILE_HANDLE *)file_handle;
    wt_fh = local_fh->fh;

    local_fh->local->fh_ops++;
    return (wt_fh->fh_sync(wt_fh, session));
}

/*
 * local_file_write --
 *     POSIX pwrite.
 */
static int
local_file_write(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, const void *buf)
{
    LOCAL_FILE_HANDLE *local_fh;
    WT_FILE_HANDLE *wt_fh;

    local_fh = (LOCAL_FILE_HANDLE *)file_handle;
    wt_fh = local_fh->fh;

    local_fh->local->write_ops++;
    return (wt_fh->fh_write(wt_fh, session, offset, len, buf));
}

/*
 * wiredtiger_extension_init --
 *     A simple shared library encryption example.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    LOCAL_STORAGE *local;
    int ret;

    if ((local = calloc(1, sizeof(LOCAL_STORAGE))) == NULL)
        return (errno);
    local->wt_api = connection->get_extension_api(connection);
    if ((ret = pthread_rwlock_init(&local->file_handle_lock, NULL)) != 0 ||
      (ret = pthread_rwlock_init(&local->flush_lock, NULL)) != 0) {
        (void)local_err(local, NULL, ret, "pthread_rwlock_init");
        free(local);
        return (ret);
    }

    /*
     * Allocate a local storage structure, with a WT_STORAGE structure as the first field, allowing
     * us to treat references to either type of structure as a reference to the other type.
     */
    local->storage_source.ss_customize_file_system = local_customize_file_system;
    local->storage_source.ss_flush = local_flush;
    local->storage_source.terminate = local_terminate;

    if ((ret = local_configure(local, config)) != 0) {
        free(local);
        return (ret);
    }

    /* Load the storage */
    if ((ret = connection->add_storage_source(
           connection, "local_store", &local->storage_source, NULL)) != 0) {
        (void)local_err(local, NULL, ret, "WT_CONNECTION->add_storage_source");
        free(local);
    }
    return (ret);
}
