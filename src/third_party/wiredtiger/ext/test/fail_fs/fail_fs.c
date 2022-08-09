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

#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <execinfo.h>

#include <wiredtiger_ext.h>
#include "queue.h"
#include "test_util.h"

#define FAIL_FS_GIGABYTE (1024 * 1024 * 1024)

#define FAIL_FS_ENV_ENABLE "WT_FAIL_FS_ENABLE"
#define FAIL_FS_ENV_WRITE_ALLOW "WT_FAIL_FS_WRITE_ALLOW"
#define FAIL_FS_ENV_READ_ALLOW "WT_FAIL_FS_READ_ALLOW"

/*
 * A "fail file system", that is, a file system extension that fails when we want it to. This is
 * only used in test frameworks, this fact allows us to simplify some error paths. This code is not
 * portable to Windows, as it has direct knowledge of file descriptors, environment variables and
 * stack traces.
 *
 * When the filesystem extension is configured, parameters can set how many reads or writes can be
 * allowed before failure. If this is not fine-grained enough, an 'environment' configuration
 * parameter can be specified. If that is used, then on every file system read or write, environment
 * variables are checked that control when reading or writing should fail.
 */
typedef struct {
    WT_FILE_SYSTEM iface;
    /*
     * WiredTiger performs schema and I/O operations in parallel, all file system and file handle
     * access must be thread-safe. This extension uses a single, global file system lock.
     */
    pthread_rwlock_t lock; /* Lock */
    bool fail_enabled;
    bool use_environment;
    bool verbose;
    int64_t read_ops;
    int64_t write_ops;
    int64_t allow_reads;
    int64_t allow_writes;
    /* Queue of file handles */
    TAILQ_HEAD(fail_file_handle_qh, fail_file_handle) fileq;
    WT_EXTENSION_API *wtext; /* Extension functions */
} FAIL_FILE_SYSTEM;

typedef struct fail_file_handle {
    WT_FILE_HANDLE iface;

    /*
     * Track the system file descriptor for each file.
     */
    FAIL_FILE_SYSTEM *fail_fs;       /* Enclosing file system */
    TAILQ_ENTRY(fail_file_handle) q; /* Queue of handles */
    int fd;                          /* System file descriptor */
} FAIL_FILE_HANDLE;

static int fail_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static void fail_file_handle_remove(WT_SESSION *, FAIL_FILE_HANDLE *);
static int fail_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int fail_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int fail_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int fail_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int fail_file_truncate(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t);
static int fail_file_write(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);
static bool fail_fs_arg(const char *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *, int64_t *);
static int fail_fs_directory_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int fail_fs_directory_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static void fail_fs_env(const char *, int64_t *);
static int fail_fs_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int fail_fs_open(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static int fail_fs_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int fail_fs_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int fail_fs_simulate_fail(FAIL_FILE_HANDLE *, WT_SESSION *, int64_t, const char *);
static int fail_fs_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);
static int fail_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);

/*
 * We use pthread functions for portable locking. Assert on errors for simplicity.
 */
static void
fail_fs_allocate_lock(pthread_rwlock_t *lockp)
{
    testutil_assert(pthread_rwlock_init(lockp, NULL) == 0);
}

static void
fail_fs_destroy_lock(pthread_rwlock_t *lockp)
{
    testutil_assert(pthread_rwlock_destroy(lockp) == 0);
}

static void
fail_fs_lock(pthread_rwlock_t *lockp)
{
    testutil_assert(pthread_rwlock_wrlock(lockp) == 0);
}

static void
fail_fs_unlock(pthread_rwlock_t *lockp)
{
    testutil_assert(pthread_rwlock_unlock(lockp) == 0);
}

/*
 * fail_file_close --
 *     ANSI C close.
 */
static int
fail_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    FAIL_FILE_HANDLE *fail_fh;
    FAIL_FILE_SYSTEM *fail_fs;
    int ret;

    (void)session; /* Unused */

    fail_fh = (FAIL_FILE_HANDLE *)file_handle;
    fail_fs = fail_fh->fail_fs;

    /*
     * We don't actually open an fd when opening directories for flushing, so ignore that case here.
     */
    if (fail_fh->fd < 0)
        return (0);
    ret = close(fail_fh->fd);
    fail_fh->fd = -1;
    fail_fs_lock(&fail_fs->lock);
    fail_file_handle_remove(session, fail_fh);
    fail_fs_unlock(&fail_fs->lock);
    return (ret);
}

/*
 * fail_file_handle_remove --
 *     Destroy an in-memory file handle. Should only happen on remove or shutdown. The file system
 *     lock must be held during this call.
 */
static void
fail_file_handle_remove(WT_SESSION *session, FAIL_FILE_HANDLE *fail_fh)
{
    FAIL_FILE_SYSTEM *fail_fs;

    (void)session; /* Unused */
    fail_fs = fail_fh->fail_fs;

    TAILQ_REMOVE(&fail_fs->fileq, fail_fh, q);

    free(fail_fh->iface.name);
    free(fail_fh);
}

/*
 * fail_file_lock --
 *     Lock/unlock a file.
 */
static int
fail_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    /* Locks are always granted. */
    (void)file_handle; /* Unused */
    (void)session;     /* Unused */
    (void)lock;        /* Unused */

    return (0);
}

/*
 * fail_file_read --
 *     POSIX pread.
 */
static int
fail_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    FAIL_FILE_HANDLE *fail_fh;
    FAIL_FILE_SYSTEM *fail_fs;
    WT_EXTENSION_API *wtext;
    int64_t envint, read_ops;
    int ret;
    size_t chunk;
    ssize_t nr;
    uint8_t *addr;

    fail_fh = (FAIL_FILE_HANDLE *)file_handle;
    fail_fs = fail_fh->fail_fs;
    wtext = fail_fs->wtext;
    read_ops = 0;
    ret = 0;

    fail_fs_lock(&fail_fs->lock);

    if (fail_fs->use_environment) {
        fail_fs_env(FAIL_FS_ENV_ENABLE, &envint);
        if (envint != 0) {
            if (!fail_fs->fail_enabled) {
                fail_fs->fail_enabled = true;
                fail_fs_env(FAIL_FS_ENV_READ_ALLOW, &fail_fs->allow_reads);
                fail_fs->read_ops = 0;
            }
            read_ops = ++fail_fs->read_ops;
        } else
            fail_fs->fail_enabled = false;
    } else
        read_ops = ++fail_fs->read_ops;

    fail_fs_unlock(&fail_fs->lock);

    if (fail_fs->fail_enabled && fail_fs->allow_reads != 0 && read_ops % fail_fs->allow_reads == 0)
        return (fail_fs_simulate_fail(fail_fh, session, read_ops, "read"));

    /* Break reads larger than 1GB into 1GB chunks. */
    for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
        chunk = (len < FAIL_FS_GIGABYTE) ? len : FAIL_FS_GIGABYTE;
        if ((nr = pread(fail_fh->fd, addr, chunk, offset)) <= 0) {
            (void)wtext->err_printf(wtext, session,
              "%s: handle-read: failed to read %" PRIuMAX " bytes at offset %" PRIuMAX ": %s",
              fail_fh->iface.name, (uintmax_t)len, (uintmax_t)offset,
              wtext->strerror(wtext, NULL, errno));
            ret = (nr == 0 ? WT_ERROR : errno);
            break;
        }
    }
    return (ret);
}

/*
 * fail_file_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
fail_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    FAIL_FILE_HANDLE *fail_fh;
    struct stat statbuf;
    int ret;

    (void)session; /* Unused */

    fail_fh = (FAIL_FILE_HANDLE *)file_handle;
    ret = 0;

    if ((ret = fstat(fail_fh->fd, &statbuf)) != 0)
        return (ret);
    *sizep = statbuf.st_size;
    return (0);
}

/*
 * fail_file_sync --
 *     Ensure the content of the file is stable. This is a no-op in our file system.
 */
static int
fail_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    (void)file_handle; /* Unused */
    (void)session;     /* Unused */

    return (0);
}

/*
 * fail_file_truncate --
 *     POSIX ftruncate.
 */
static int
fail_file_truncate(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset)
{
    FAIL_FILE_HANDLE *fail_fh;

    (void)session; /* Unused */

    fail_fh = (FAIL_FILE_HANDLE *)file_handle;
    return (ftruncate(fail_fh->fd, offset));
}

/*
 * fail_file_write --
 *     POSIX pwrite.
 */
static int
fail_file_write(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, const void *buf)
{
    FAIL_FILE_HANDLE *fail_fh;
    FAIL_FILE_SYSTEM *fail_fs;
    WT_EXTENSION_API *wtext;
    int64_t envint, write_ops;
    int ret;
    size_t chunk;
    ssize_t nr;
    const uint8_t *addr;

    fail_fh = (FAIL_FILE_HANDLE *)file_handle;
    fail_fs = fail_fh->fail_fs;
    wtext = fail_fs->wtext;
    write_ops = 0;
    ret = 0;

    fail_fs_lock(&fail_fs->lock);

    if (fail_fs->use_environment) {
        fail_fs_env(FAIL_FS_ENV_ENABLE, &envint);
        if (envint != 0) {
            if (!fail_fs->fail_enabled) {
                fail_fs->fail_enabled = true;
                fail_fs_env(FAIL_FS_ENV_WRITE_ALLOW, &fail_fs->allow_writes);
                fail_fs->write_ops = 0;
            }
            write_ops = ++fail_fs->write_ops;
        } else
            fail_fs->fail_enabled = false;
    } else
        write_ops = ++fail_fs->write_ops;

    fail_fs_unlock(&fail_fs->lock);

    if (fail_fs->fail_enabled && fail_fs->allow_writes != 0 &&
      write_ops % fail_fs->allow_writes == 0)
        return (fail_fs_simulate_fail(fail_fh, session, write_ops, "write"));

    /* Break writes larger than 1GB into 1GB chunks. */
    for (addr = buf; len > 0; addr += nr, len -= (size_t)nr, offset += nr) {
        chunk = (len < FAIL_FS_GIGABYTE) ? len : FAIL_FS_GIGABYTE;
        if ((nr = pwrite(fail_fh->fd, addr, chunk, offset)) <= 0) {
            (void)wtext->err_printf(wtext, session,
              "%s: handle-write: failed to write %" PRIuMAX " bytes at offset %" PRIuMAX ": %s",
              fail_fh->iface.name, (uintmax_t)len, (uintmax_t)offset,
              wtext->strerror(wtext, NULL, errno));
            ret = (nr == 0 ? WT_ERROR : errno);
            break;
        }
    }
    return (ret);
}

/*
 * fail_fs_arg --
 *     If the key matches, return the value interpreted as an integer.
 */
static bool
fail_fs_arg(const char *match, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value, int64_t *argp)
{
    if (strncmp(match, key->str, key->len) == 0 && match[key->len] == '\0' &&
      (value->type == WT_CONFIG_ITEM_BOOL || value->type == WT_CONFIG_ITEM_NUM)) {
        *argp = value->val;
        return (true);
    }
    return (false);
}

/*
 * fail_fs_directory_list --
 *     Return a list of files in a given sub-directory.
 */
static int
fail_fs_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    FAIL_FILE_HANDLE *fail_fh;
    FAIL_FILE_SYSTEM *fail_fs;
    size_t len, prefix_len;
    uint32_t allocated, count;
    int ret;
    char *name, **entries;
    void *p;

    (void)session; /* Unused */

    fail_fs = (FAIL_FILE_SYSTEM *)file_system;
    ret = 0;
    *dirlistp = NULL;
    *countp = 0;

    entries = NULL;
    allocated = count = 0;
    len = strlen(directory);
    prefix_len = prefix == NULL ? 0 : strlen(prefix);

    fail_fs_lock(&fail_fs->lock);
    TAILQ_FOREACH (fail_fh, &fail_fs->fileq, q) {
        name = fail_fh->iface.name;
        if (strncmp(name, directory, len) != 0 ||
          (prefix != NULL && strncmp(name, prefix, prefix_len) != 0))
            continue;

        /*
         * Increase the list size in groups of 10, it doesn't matter if the list is a bit longer
         * than necessary.
         */
        if (count >= allocated) {
            allocated += 10;
            if ((p = realloc(entries, allocated * sizeof(*entries))) == NULL) {
                ret = ENOMEM;
                goto err;
            }
            entries = p;
            memset(entries + count, 0, 10 * sizeof(*entries));
        }
        entries[count++] = strdup(name);
    }

    *dirlistp = entries;
    *countp = count;

err:
    fail_fs_unlock(&fail_fs->lock);
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
 * fail_fs_directory_list_free --
 *     Free memory allocated by fail_fs_directory_list.
 */
static int
fail_fs_directory_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    (void)file_system; /* Unused */
    (void)session;     /* Unused */

    if (dirlist != NULL) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }
    return (0);
}

/*
 * fail_fs_env --
 *     If the name is in the environment, return its integral value.
 */
static void
fail_fs_env(const char *name, int64_t *valp)
{
    int64_t result;
    char *s, *value;

    result = 0;
    if ((value = getenv(name)) != NULL) {
        s = value;
        if (strcmp(value, "true") == 0)
            result = 1;
        else if (strcmp(value, "false") != 0) {
            result = strtoll(value, &s, 10);
            if (*s != '\0')
                result = 0;
        }
    }
    *valp = result;
}

/*
 * fail_fs_exist --
 *     Return if the file exists.
 */
static int
fail_fs_exist(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    (void)file_system; /* Unused */
    (void)session;     /* Unused */

    *existp = (access(name, F_OK) == 0);
    return (0);
}

/*
 * fail_fs_open --
 *     fopen for the fail file system.
 */
static int
fail_fs_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    FAIL_FILE_HANDLE *fail_fh;
    FAIL_FILE_SYSTEM *fail_fs;
    WT_EXTENSION_API *wtext;
    WT_FILE_HANDLE *file_handle;
    int fd, open_flags, ret;

    (void)session; /* Unused */

    *file_handlep = NULL;

    fail_fh = NULL;
    fail_fs = (FAIL_FILE_SYSTEM *)file_system;
    fd = -1;
    ret = 0;

    if (fail_fs->verbose) {
        wtext = fail_fs->wtext;
        (void)wtext->msg_printf(wtext, session, "fail_fs: open: %s", name);
    }

    fail_fs_lock(&fail_fs->lock);

    open_flags = 0;
    if ((flags & WT_FS_OPEN_CREATE) != 0)
        open_flags |= O_CREAT;
    if ((flags & WT_FS_OPEN_EXCLUSIVE) != 0)
        open_flags |= O_EXCL;
    if ((flags & WT_FS_OPEN_READONLY) != 0)
        open_flags |= O_RDONLY;
    else
        open_flags |= O_RDWR;

    /*
     * Opening a file handle on a directory is only to support filesystems that require a directory
     * sync for durability. This is a no-op for this file system.
     */
    if (file_type == WT_FS_OPEN_FILE_TYPE_DIRECTORY)
        fd = -1;
    else if ((fd = open(name, open_flags, 0666)) < 0) {
        ret = errno;
        goto err;
    }

    /* We create a handle structure for each open. */
    if ((fail_fh = calloc(1, sizeof(FAIL_FILE_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    /* Initialize private information. */
    fail_fh->fail_fs = fail_fs;
    fail_fh->fd = fd;

    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)fail_fh;
    if ((file_handle->name = strdup(name)) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    /* Setup the function call table. */
    file_handle->close = fail_file_close;
    file_handle->fh_advise = NULL;
    file_handle->fh_extend = NULL;
    file_handle->fh_extend_nolock = NULL;
    file_handle->fh_lock = fail_file_lock;
    file_handle->fh_map = NULL;
    file_handle->fh_map_discard = NULL;
    file_handle->fh_map_preload = NULL;
    file_handle->fh_unmap = NULL;
    file_handle->fh_read = fail_file_read;
    file_handle->fh_size = fail_file_size;
    file_handle->fh_sync = fail_file_sync;
    file_handle->fh_sync_nowait = NULL;
    file_handle->fh_truncate = fail_file_truncate;
    file_handle->fh_write = fail_file_write;

    TAILQ_INSERT_HEAD(&fail_fs->fileq, fail_fh, q);

    *file_handlep = file_handle;

    if (0) {
err:
        if (fd != -1)
            (void)close(fd);
        free(fail_fh);
    }

    fail_fs_unlock(&fail_fs->lock);
    return (ret);
}

/*
 * fail_fs_remove --
 *     POSIX remove.
 */
static int
fail_fs_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    (void)file_system; /* Unused */
    (void)session;     /* Unused */
    (void)flags;       /* Unused */

    return (unlink(name));
}

/*
 * fail_fs_rename --
 *     POSIX rename.
 */
static int
fail_fs_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    (void)file_system; /* Unused */
    (void)session;     /* Unused */
    (void)flags;       /* Unused */

    return (rename(from, to));
}

/*
 * fail_fs_simulate_fail --
 *     Simulate a failure from this file system by reporting it and returning a non-zero return
 *     code.
 */
static int
fail_fs_simulate_fail(
  FAIL_FILE_HANDLE *fail_fh, WT_SESSION *session, int64_t nops, const char *opkind)
{
    FAIL_FILE_SYSTEM *fail_fs;
    WT_EXTENSION_API *wtext;
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    size_t btret, i;
#else
    int btret, i;
#endif
    void *bt[100];
    char **btstr;

    fail_fs = fail_fh->fail_fs;
    if (fail_fs->verbose) {
        wtext = fail_fs->wtext;
        (void)wtext->msg_printf(wtext, session,
          "fail_fs: %s: simulated failure after %" PRId64 " %s operations", fail_fh->iface.name,
          nops, opkind);
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        btret = backtrace(bt, sizeof(bt) / sizeof(bt[0]));
#else
        btret = backtrace(bt, (int)(sizeof(bt) / sizeof(bt[0])));
#endif
        if ((btstr = backtrace_symbols(bt, btret)) != NULL) {
            for (i = 0; i < btret; i++)
                (void)wtext->msg_printf(wtext, session, "  %s", btstr[i]);
            free(btstr);
        }
    }
    return (EIO);
}

/*
 * fail_fs_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
fail_fs_size(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    struct stat statbuf;
    int ret;

    (void)file_system; /* Unused */
    (void)session;     /* Unused */

    ret = 0;
    if ((ret = stat(name, &statbuf)) != 0)
        return (ret);
    *sizep = statbuf.st_size;
    return (0);
}

/*
 * fail_fs_terminate --
 *     Discard any resources on termination
 */
static int
fail_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    FAIL_FILE_HANDLE *fail_fh, *fail_fh_tmp;
    FAIL_FILE_SYSTEM *fail_fs;

    fail_fs = (FAIL_FILE_SYSTEM *)file_system;

    TAILQ_FOREACH_SAFE(fail_fh, &fail_fs->fileq, q, fail_fh_tmp)
    fail_file_handle_remove(session, fail_fh);

    fail_fs_destroy_lock(&fail_fs->lock);
    free(fail_fs);

    return (0);
}

/*
 * wiredtiger_extension_init --
 *     WiredTiger fail filesystem extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
    FAIL_FILE_SYSTEM *fail_fs;
    WT_CONFIG_ITEM k, v;
    WT_CONFIG_PARSER *config_parser;
    WT_EXTENSION_API *wtext;
    WT_FILE_SYSTEM *file_system;
    int64_t argval;
    int ret;

    config_parser = NULL;
    wtext = conn->get_extension_api(conn);
    ret = 0;

    if ((fail_fs = calloc(1, sizeof(FAIL_FILE_SYSTEM))) == NULL) {
        (void)wtext->err_printf(
          wtext, NULL, "fail_file_system extension_init: %s", wtext->strerror(wtext, NULL, ENOMEM));
        return (ENOMEM);
    }
    fail_fs->wtext = wtext;
    file_system = (WT_FILE_SYSTEM *)fail_fs;

    /* Get any configuration values. */
    if ((ret = wtext->config_parser_open_arg(wtext, NULL, config, &config_parser)) != 0) {
        (void)wtext->err_printf(wtext, NULL, "WT_EXTENSION_API.config_parser_open: config: %s",
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
        if (fail_fs_arg("environment", &k, &v, &argval)) {
            fail_fs->use_environment = (argval != 0);
            continue;
        } else if (fail_fs_arg("verbose", &k, &v, &argval)) {
            fail_fs->verbose = (argval != 0);
            continue;
        } else if (fail_fs_arg("allow_writes", &k, &v, &fail_fs->allow_writes))
            continue;
        else if (fail_fs_arg("allow_reads", &k, &v, &fail_fs->allow_reads))
            continue;

        (void)wtext->err_printf(wtext, NULL,
          "WT_CONFIG_PARSER.next: unexpected configuration information: %.*s=%.*s: %s", (int)k.len,
          k.str, (int)v.len, v.str, wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    if (ret != WT_NOTFOUND) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONFIG_PARSER.next: config: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    ret = config_parser->close(config_parser);
    config_parser = NULL;
    if (ret != 0) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONFIG_PARSER.close: config: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    if (fail_fs->allow_writes != 0 || fail_fs->allow_reads != 0)
        fail_fs->fail_enabled = true;

    fail_fs_allocate_lock(&fail_fs->lock);
    /* Initialize the in-memory jump table. */
    file_system->fs_directory_list = fail_fs_directory_list;
    file_system->fs_directory_list_free = fail_fs_directory_list_free;
    file_system->fs_exist = fail_fs_exist;
    file_system->fs_open_file = fail_fs_open;
    file_system->fs_remove = fail_fs_remove;
    file_system->fs_rename = fail_fs_rename;
    file_system->fs_size = fail_fs_size;
    file_system->terminate = fail_fs_terminate;
    if ((ret = conn->set_file_system(conn, file_system, NULL)) != 0) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONNECTION.set_file_system: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    return (0);

err:
    if (config_parser != NULL)
        (void)config_parser->close(config_parser);
    free(fail_fs);
    return (ret);
}
