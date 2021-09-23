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
 *
 * ex_file_system.c
 * 	demonstrates how to use the custom file system interface
 */
#include <test_util.h>

/*
 * This example code uses pthread functions for portable locking, we ignore errors for simplicity.
 */
static void
allocate_file_system_lock(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_init(lockp, NULL));
}

static void
destroy_file_system_lock(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_destroy(lockp));
}

static void
lock_file_system(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_wrlock(lockp));
}

static void
unlock_file_system(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_unlock(lockp));
}

/*
 * Example file system implementation, using memory buffers to represent files.
 */
typedef struct {
    WT_FILE_SYSTEM iface;

    /*
     * WiredTiger performs schema and I/O operations in parallel, all file system and file handle
     * access must be thread-safe. This example uses a single, global file system lock for
     * simplicity; real applications might require finer granularity, for example, a single lock for
     * the file system handle list and per-handle locks serializing I/O.
     */
    pthread_rwlock_t lock; /* Lock */

    int opened_file_count;
    int opened_unique_file_count;
    int closed_file_count;
    int read_ops;
    int write_ops;

    /* Queue of file handles */
    TAILQ_HEAD(demo_file_handle_qh, demo_file_handle) fileq;

    WT_EXTENSION_API *wtext; /* Extension functions */

} DEMO_FILE_SYSTEM;

typedef struct demo_file_handle {
    WT_FILE_HANDLE iface;

    /*
     * Add custom file handle fields after the interface.
     */
    DEMO_FILE_SYSTEM *demo_fs; /* Enclosing file system */

    TAILQ_ENTRY(demo_file_handle) q; /* Queue of handles */
    uint32_t ref;                    /* Reference count */

    char *buf;      /* In-memory contents */
    size_t bufsize; /* In-memory buffer size */

    size_t size; /* Read/write data size */
} DEMO_FILE_HANDLE;

/*
 * Extension initialization function.
 */
#ifdef _WIN32
/*
 * Explicitly export this function so it is visible when loading extensions.
 */
__declspec(dllexport)
#endif
  int demo_file_system_create(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * Forward function declarations for file system API implementation
 */
static int demo_fs_open(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static int demo_fs_directory_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int demo_fs_directory_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int demo_fs_exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int demo_fs_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int demo_fs_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int demo_fs_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);
static int demo_fs_terminate(WT_FILE_SYSTEM *, WT_SESSION *);

/*
 * Forward function declarations for file handle API implementation
 */
static int demo_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int demo_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int demo_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int demo_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_truncate(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t);
static int demo_file_write(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);

/*
 * Forward function declarations for internal functions
 */
static int demo_handle_remove(WT_SESSION *, DEMO_FILE_HANDLE *);
static DEMO_FILE_HANDLE *demo_handle_search(WT_FILE_SYSTEM *, const char *);

#define DEMO_FILE_SIZE_INCREMENT 32768

/*
 * string_match --
 *     Return if a string matches a byte string of len bytes.
 */
static bool
byte_string_match(const char *str, const char *bytes, size_t len)
{
    return (strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0');
}

/*
 * demo_file_system_create --
 *     Initialization point for demo file system
 */
int
demo_file_system_create(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
    DEMO_FILE_SYSTEM *demo_fs;
    WT_CONFIG_ITEM k, v;
    WT_CONFIG_PARSER *config_parser;
    WT_EXTENSION_API *wtext;
    WT_FILE_SYSTEM *file_system;
    int ret = 0;

    wtext = conn->get_extension_api(conn);

    if ((demo_fs = calloc(1, sizeof(DEMO_FILE_SYSTEM))) == NULL) {
        (void)wtext->err_printf(
          wtext, NULL, "demo_file_system_create: %s", wtext->strerror(wtext, NULL, ENOMEM));
        return (ENOMEM);
    }
    demo_fs->wtext = wtext;
    file_system = (WT_FILE_SYSTEM *)demo_fs;

    /*
     * Applications may have their own configuration information to pass to the underlying
     * filesystem implementation. See the main function for the setup of those configuration
     * strings; here we parse configuration information as passed in by main, through WiredTiger.
     */
    if ((ret = wtext->config_parser_open_arg(wtext, NULL, config, &config_parser)) != 0) {
        (void)wtext->err_printf(wtext, NULL, "WT_EXTENSION_API.config_parser_open: config: %s",
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    /* Step through our configuration values. */
    printf("Custom file system configuration\n");
    while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
        if (byte_string_match("config_string", k.str, k.len)) {
            printf(
              "\t"
              "key %.*s=\"%.*s\"\n",
              (int)k.len, k.str, (int)v.len, v.str);
            continue;
        }
        if (byte_string_match("config_value", k.str, k.len)) {
            printf(
              "\t"
              "key %.*s=%" PRId64 "\n",
              (int)k.len, k.str, v.val);
            continue;
        }
        ret = EINVAL;
        (void)wtext->err_printf(wtext, NULL,
          "WT_CONFIG_PARSER.next: unexpected configuration "
          "information: %.*s=%.*s: %s",
          (int)k.len, k.str, (int)v.len, v.str, wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    /* Check for expected parser termination and close the parser. */
    if (ret != WT_NOTFOUND) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONFIG_PARSER.next: config: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    if ((ret = config_parser->close(config_parser)) != 0) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONFIG_PARSER.close: config: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    allocate_file_system_lock(&demo_fs->lock);
    /*! [WT_FILE_SYSTEM create] */
    /* Initialize the in-memory jump table. */
    file_system->fs_directory_list = demo_fs_directory_list;
    file_system->fs_directory_list_free = demo_fs_directory_list_free;
    file_system->fs_exist = demo_fs_exist;
    file_system->fs_open_file = demo_fs_open;
    file_system->fs_remove = demo_fs_remove;
    file_system->fs_rename = demo_fs_rename;
    file_system->fs_size = demo_fs_size;
    file_system->terminate = demo_fs_terminate;

    if ((ret = conn->set_file_system(conn, file_system, NULL)) != 0) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONNECTION.set_file_system: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }
    /*! [WT_FILE_SYSTEM create] */
    return (0);

err:
    free(demo_fs);
    /* An error installing the file system is fatal. */
    exit(1);
}

/*
 * demo_fs_open --
 *     fopen for our demo file system
 */
static int
demo_fs_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    WT_EXTENSION_API *wtext;
    WT_FILE_HANDLE *file_handle;
    int ret = 0;

    (void)file_type; /* Unused */
    (void)flags;     /* Unused */

    *file_handlep = NULL;

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;
    demo_fh = NULL;
    wtext = demo_fs->wtext;

    lock_file_system(&demo_fs->lock);
    ++demo_fs->opened_file_count;

    /*
     * First search the file queue, if we find it, assert there's only a single reference, we only
     * support a single handle on any file.
     */
    demo_fh = demo_handle_search(file_system, name);
    if (demo_fh != NULL) {
        if (demo_fh->ref != 0) {
            (void)wtext->err_printf(wtext, session, "demo_fs_open: %s: file already open", name);
            ret = EBUSY;
            goto err;
        }

        demo_fh->ref = 1;

        *file_handlep = (WT_FILE_HANDLE *)demo_fh;

        unlock_file_system(&demo_fs->lock);
        return (0);
    }

    /* The file hasn't been opened before, create a new one. */
    if ((demo_fh = calloc(1, sizeof(DEMO_FILE_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    /* Initialize private information. */
    demo_fh->demo_fs = demo_fs;
    demo_fh->ref = 1;
    if ((demo_fh->buf = calloc(1, DEMO_FILE_SIZE_INCREMENT)) == NULL) {
        ret = ENOMEM;
        goto err;
    }
    demo_fh->bufsize = DEMO_FILE_SIZE_INCREMENT;
    demo_fh->size = 0;

    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)demo_fh;
    if ((file_handle->name = strdup(name)) == NULL) {
        ret = ENOMEM;
        goto err;
    }
    /*! [WT_FILE_HANDLE create] */
    /*
     * Setup the function call table for our custom file system. Set the function pointer to NULL
     * where our implementation doesn't support the functionality.
     */
    file_handle->close = demo_file_close;
    file_handle->fh_advise = NULL;
    file_handle->fh_extend = NULL;
    file_handle->fh_extend_nolock = NULL;
    file_handle->fh_lock = demo_file_lock;
    file_handle->fh_map = NULL;
    file_handle->fh_map_discard = NULL;
    file_handle->fh_map_preload = NULL;
    file_handle->fh_unmap = NULL;
    file_handle->fh_read = demo_file_read;
    file_handle->fh_size = demo_file_size;
    file_handle->fh_sync = demo_file_sync;
    file_handle->fh_sync_nowait = NULL;
    file_handle->fh_truncate = demo_file_truncate;
    file_handle->fh_write = demo_file_write;

    TAILQ_INSERT_HEAD(&demo_fs->fileq, demo_fh, q);
    ++demo_fs->opened_unique_file_count;

    *file_handlep = file_handle;
    /*! [WT_FILE_HANDLE create] */

    if (0) {
err:
        free(demo_fh->buf);
        free(demo_fh);
    }

    unlock_file_system(&demo_fs->lock);
    return (ret);
}

/*
 * demo_fs_directory_list --
 *     Return a list of files in a given sub-directory.
 */
static int
demo_fs_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    size_t len, prefix_len;
    uint32_t allocated, count;
    int ret = 0;
    char *name, **entries;
    void *p;

    (void)session; /* Unused */

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    *dirlistp = NULL;
    *countp = 0;

    entries = NULL;
    allocated = count = 0;
    len = strlen(directory);
    prefix_len = prefix == NULL ? 0 : strlen(prefix);

    lock_file_system(&demo_fs->lock);
    TAILQ_FOREACH (demo_fh, &demo_fs->fileq, q) {
        name = demo_fh->iface.name;
        if (strncmp(name, directory, len) != 0 ||
          (prefix != NULL && strncmp(name, prefix, prefix_len) != 0))
            continue;

        /*
         * Increase the list size in groups of 10, it doesn't matter if the list is a bit longer
         * than necessary.
         */
        if (count >= allocated) {
            p = realloc(entries, (allocated + 10) * sizeof(*entries));
            if (p == NULL) {
                ret = ENOMEM;
                goto err;
            }

            entries = p;
            memset(entries + allocated * sizeof(*entries), 0, 10 * sizeof(*entries));
            allocated += 10;
        }
        entries[count++] = strdup(name);
    }

    *dirlistp = entries;
    *countp = count;

err:
    unlock_file_system(&demo_fs->lock);
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
 * demo_fs_directory_list_free --
 *     Free memory allocated by demo_fs_directory_list.
 */
static int
demo_fs_directory_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    (void)file_system;
    (void)session;

    if (dirlist != NULL) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }
    return (0);
}

/*
 * demo_fs_exist --
 *     Return if the file exists.
 */
static int
demo_fs_exist(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    DEMO_FILE_SYSTEM *demo_fs;

    (void)session; /* Unused */

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    lock_file_system(&demo_fs->lock);
    *existp = demo_handle_search(file_system, name) != NULL;
    unlock_file_system(&demo_fs->lock);

    return (0);
}

/*
 * demo_fs_remove --
 *     POSIX remove.
 */
static int
demo_fs_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    DEMO_FILE_SYSTEM *demo_fs;
    DEMO_FILE_HANDLE *demo_fh;
    int ret = 0;

    (void)session; /* Unused */
    (void)flags;   /* Unused */

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    ret = ENOENT;
    lock_file_system(&demo_fs->lock);
    if ((demo_fh = demo_handle_search(file_system, name)) != NULL)
        ret = demo_handle_remove(session, demo_fh);
    unlock_file_system(&demo_fs->lock);

    return (ret);
}

/*
 * demo_fs_rename --
 *     POSIX rename.
 */
static int
demo_fs_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    char *copy;
    int ret = 0;

    (void)session; /* Unused */
    (void)flags;   /* Unused */

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    lock_file_system(&demo_fs->lock);
    if ((demo_fh = demo_handle_search(file_system, from)) == NULL)
        ret = ENOENT;
    else if ((copy = strdup(to)) == NULL)
        ret = ENOMEM;
    else {
        free(demo_fh->iface.name);
        demo_fh->iface.name = copy;
    }
    unlock_file_system(&demo_fs->lock);
    return (ret);
}

/*
 * demo_fs_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
demo_fs_size(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    DEMO_FILE_SYSTEM *demo_fs;
    DEMO_FILE_HANDLE *demo_fh;
    int ret = 0;

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    ret = ENOENT;
    lock_file_system(&demo_fs->lock);
    if ((demo_fh = demo_handle_search(file_system, name)) != NULL) {
        unlock_file_system(&demo_fs->lock);
        ret = demo_file_size((WT_FILE_HANDLE *)demo_fh, session, sizep);
    } else {
        unlock_file_system(&demo_fs->lock);
    }

    return (ret);
}

/*
 * demo_fs_terminate --
 *     Discard any resources on termination
 */
static int
demo_fs_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    DEMO_FILE_HANDLE *demo_fh, *demo_fh_tmp;
    DEMO_FILE_SYSTEM *demo_fs;
    int ret = 0, tret;

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    TAILQ_FOREACH_SAFE(demo_fh, &demo_fs->fileq, q, demo_fh_tmp)
    if ((tret = demo_handle_remove(session, demo_fh)) != 0 && ret == 0)
        ret = tret;

    printf("Custom file system\n");
    printf("\t%d unique file opens\n", demo_fs->opened_unique_file_count);
    printf("\t%d files opened\n", demo_fs->opened_file_count);
    printf("\t%d files closed\n", demo_fs->closed_file_count);
    printf("\t%d reads, %d writes\n", demo_fs->read_ops, demo_fs->write_ops);

    destroy_file_system_lock(&demo_fs->lock);
    free(demo_fs);

    return (ret);
}

/*
 * demo_file_close --
 *     ANSI C close.
 */
static int
demo_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;

    (void)session; /* Unused */

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_fs = demo_fh->demo_fs;

    lock_file_system(&demo_fs->lock);
    if (--demo_fh->ref == 0)
        ++demo_fs->closed_file_count;
    unlock_file_system(&demo_fs->lock);

    return (0);
}

/*
 * demo_file_lock --
 *     Lock/unlock a file.
 */
static int
demo_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    /* Locks are always granted. */
    (void)file_handle; /* Unused */
    (void)session;     /* Unused */
    (void)lock;        /* Unused */
    return (0);
}

/*
 * demo_file_read --
 *     POSIX pread.
 */
static int
demo_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    WT_EXTENSION_API *wtext;
    size_t off;
    int ret = 0;

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_fs = demo_fh->demo_fs;
    wtext = demo_fs->wtext;
    off = (size_t)offset;

    lock_file_system(&demo_fs->lock);
    ++demo_fs->read_ops;
    if (off < demo_fh->size) {
        if (len > demo_fh->size - off)
            len = demo_fh->size - off;
        memcpy(buf, (uint8_t *)demo_fh->buf + off, len);
    } else
        ret = EIO; /* EOF */
    unlock_file_system(&demo_fs->lock);
    if (ret == 0)
        return (0);

    (void)wtext->err_printf(wtext, session,
      "%s: handle-read: failed to read %zu bytes at offset %zu: %s", demo_fh->iface.name, len, off,
      wtext->strerror(wtext, NULL, ret));
    return (ret);
}

/*
 * demo_file_size --
 *     Get the size of a file in bytes, by file handle.
 */
static int
demo_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;

    (void)session; /* Unused */

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_fs = demo_fh->demo_fs;

    lock_file_system(&demo_fs->lock);
    *sizep = (wt_off_t)demo_fh->size;
    unlock_file_system(&demo_fs->lock);
    return (0);
}

/*
 * demo_file_sync --
 *     Ensure the content of the file is stable. This is a no-op in our memory backed file system.
 */
static int
demo_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    (void)file_handle; /* Unused */
    (void)session;     /* Unused */

    return (0);
}

/*
 * demo_buffer_resize --
 *     Resize the write buffer.
 */
static int
demo_buffer_resize(WT_SESSION *session, DEMO_FILE_HANDLE *demo_fh, wt_off_t offset)
{
    DEMO_FILE_SYSTEM *demo_fs;
    WT_EXTENSION_API *wtext;
    size_t off;
    void *p;

    demo_fs = demo_fh->demo_fs;
    wtext = demo_fs->wtext;
    off = (size_t)offset;

    /* Grow the buffer as necessary and clear any new space in the file. */
    if (demo_fh->bufsize >= off)
        return (0);

    if ((p = realloc(demo_fh->buf, off)) == NULL) {
        (void)wtext->err_printf(wtext, session, "%s: failed to resize buffer", demo_fh->iface.name,
          wtext->strerror(wtext, NULL, ENOMEM));
        return (ENOMEM);
    }
    memset((uint8_t *)p + demo_fh->bufsize, 0, off - demo_fh->bufsize);
    demo_fh->buf = p;
    demo_fh->bufsize = off;

    return (0);
}

/*
 * demo_file_truncate --
 *     POSIX ftruncate.
 */
static int
demo_file_truncate(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    int ret = 0;

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_fs = demo_fh->demo_fs;

    lock_file_system(&demo_fs->lock);
    if ((ret = demo_buffer_resize(session, demo_fh, offset)) == 0)
        demo_fh->size = (size_t)offset;
    unlock_file_system(&demo_fs->lock);
    return (ret);
}

/*
 * demo_file_write --
 *     POSIX pwrite.
 */
static int
demo_file_write(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, const void *buf)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;
    WT_EXTENSION_API *wtext;
    size_t off;
    int ret = 0;

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_fs = demo_fh->demo_fs;
    wtext = demo_fs->wtext;
    off = (size_t)offset;

    lock_file_system(&demo_fs->lock);
    ++demo_fs->write_ops;
    if ((ret = demo_buffer_resize(
           session, demo_fh, offset + (wt_off_t)(len + DEMO_FILE_SIZE_INCREMENT))) == 0) {
        memcpy((uint8_t *)demo_fh->buf + off, buf, len);
        if (off + len > demo_fh->size)
            demo_fh->size = off + len;
    }
    unlock_file_system(&demo_fs->lock);
    if (ret == 0)
        return (0);

    (void)wtext->err_printf(wtext, session,
      "%s: handle-write: failed to write %zu bytes at offset %zu: %s", demo_fh->iface.name, len,
      off, wtext->strerror(wtext, NULL, ret));
    return (ret);
}

/*
 * demo_handle_remove --
 *     Destroy an in-memory file handle. Should only happen on remove or shutdown.
 */
static int
demo_handle_remove(WT_SESSION *session, DEMO_FILE_HANDLE *demo_fh)
{
    DEMO_FILE_SYSTEM *demo_fs;
    WT_EXTENSION_API *wtext;

    demo_fs = demo_fh->demo_fs;
    wtext = demo_fs->wtext;

    if (demo_fh->ref != 0) {
        (void)wtext->err_printf(wtext, session, "demo_handle_remove: %s: file is currently open",
          demo_fh->iface.name, wtext->strerror(wtext, NULL, EBUSY));
        return (EBUSY);
    }

    TAILQ_REMOVE(&demo_fs->fileq, demo_fh, q);

    /* Clean up private information. */
    free(demo_fh->buf);

    /* Clean up public information. */
    free(demo_fh->iface.name);

    free(demo_fh);

    return (0);
}

/*
 * demo_handle_search --
 *     Return a matching handle, if one exists.
 */
static DEMO_FILE_HANDLE *
demo_handle_search(WT_FILE_SYSTEM *file_system, const char *name)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_FILE_SYSTEM *demo_fs;

    demo_fs = (DEMO_FILE_SYSTEM *)file_system;

    TAILQ_FOREACH (demo_fh, &demo_fs->fileq, q)
        if (strcmp(demo_fh->iface.name, name) == 0)
            break;
    return (demo_fh);
}

static const char *home;

int
main(void)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    const char *key, *open_config, *uri;
    int i;
    int ret = 0;
    char kbuf[64];

    /*
     * Create a clean test directory for this run of the test program if the environment variable
     * isn't already set (as is done by make check).
     */
    if (getenv("WIREDTIGER_HOME") == NULL) {
        home = "WT_HOME";
        ret = system("rm -rf WT_HOME && mkdir WT_HOME");
    } else
        home = NULL;

    /*! [WT_FILE_SYSTEM register] */
    /*
     * Setup a configuration string that will load our custom file system. Use the special local
     * extension to indicate that the entry point is in the same executable. Also enable early load
     * for this extension, since WiredTiger needs to be able to find it before doing any file
     * operations. Finally, pass in two pieces of configuration information to our initialization
     * function as the "config" value.
     */
    open_config =
      "create,log=(enabled=true),extensions=(local={entry=demo_file_system_create,early_load=true,"
      "config={config_string=\"demo-file-system\",config_value=37}})";
    /* Open a connection to the database, creating it if necessary. */
    if ((ret = wiredtiger_open(home, NULL, open_config, &conn)) != 0) {
        fprintf(stderr, "Error connecting to %s: %s\n", home == NULL ? "." : home,
          wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    /*! [WT_FILE_SYSTEM register] */

    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
        fprintf(stderr, "WT_CONNECTION.open_session: %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    uri = "table:fs";
    if ((ret = session->create(session, uri, "key_format=S,value_format=S")) != 0) {
        fprintf(stderr, "WT_SESSION.create: %s: %s\n", uri, wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    if ((ret = session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
        fprintf(stderr, "WT_SESSION.open_cursor: %s: %s\n", uri, wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    for (i = 0; i < 1000; ++i) {
        (void)snprintf(kbuf, sizeof(kbuf), "%010d KEY -----", i);
        cursor->set_key(cursor, kbuf);
        cursor->set_value(cursor, "--- VALUE ---");
        if ((ret = cursor->insert(cursor)) != 0) {
            fprintf(stderr, "WT_CURSOR.insert: %s: %s\n", kbuf, wiredtiger_strerror(ret));
            return (EXIT_FAILURE);
        }
    }
    if ((ret = cursor->close(cursor)) != 0) {
        fprintf(stderr, "WT_CURSOR.close: %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    if ((ret = session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
        fprintf(stderr, "WT_SESSION.open_cursor: %s: %s\n", uri, wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    for (i = 0; i < 1000; ++i) {
        if ((ret = cursor->next(cursor)) != 0) {
            fprintf(stderr, "WT_CURSOR.insert: %s: %s\n", kbuf, wiredtiger_strerror(ret));
            return (EXIT_FAILURE);
        }
        (void)snprintf(kbuf, sizeof(kbuf), "%010d KEY -----", i);
        if ((ret = cursor->get_key(cursor, &key)) != 0) {
            fprintf(stderr, "WT_CURSOR.get_key: %s\n", wiredtiger_strerror(ret));
            return (EXIT_FAILURE);
        }
        if (strcmp(kbuf, key) != 0) {
            fprintf(stderr, "Key mismatch: %s, %s\n", kbuf, key);
            return (EXIT_FAILURE);
        }
    }
    if ((ret = cursor->next(cursor)) != WT_NOTFOUND) {
        fprintf(
          stderr, "WT_CURSOR.insert: expected WT_NOTFOUND, got %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    if ((ret = conn->close(conn, NULL)) != 0) {
        fprintf(stderr, "Error closing connection to %s: %s\n", home == NULL ? "." : home,
          wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    return (EXIT_SUCCESS);
}
