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
 * ex_storage_source.c
 * 	demonstrates how to use the custom storage source interface
 */
#include <test_util.h>

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

/*
 * This example code uses pthread functions for portable locking, we ignore errors for simplicity.
 */
static void
allocate_storage_source_lock(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_init(lockp, NULL));
}

static void
destroy_storage_source_lock(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_destroy(lockp));
}

static void
lock_storage_source(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_wrlock(lockp));
}

static void
unlock_storage_source(pthread_rwlock_t *lockp)
{
    error_check(pthread_rwlock_unlock(lockp));
}

/*
 * Example storage source implementation, using memory buffers to represent objects.
 */
typedef struct {
    WT_STORAGE_SOURCE iface;

    /*
     * WiredTiger performs schema and I/O operations in parallel, all storage sources and file
     * handle access must be thread-safe. This example uses a single, global storage source lock for
     * simplicity; real applications might require finer granularity, for example, a single lock for
     * the storage source handle list and per-handle locks serializing I/O.
     */
    pthread_rwlock_t lock; /* Lock */

    int closed_object_count;
    int opened_object_count;
    int opened_unique_object_count;
    int read_ops;
    int write_ops;

    /* Queue of file handles */
    TAILQ_HEAD(demo_file_handle_qh, demo_file_handle) fileq;

    WT_EXTENSION_API *wtext; /* Extension functions */

} DEMO_STORAGE_SOURCE;

typedef struct demo_file_handle {
    WT_FILE_HANDLE iface;

    /*
     * Add custom file handle fields after the interface.
     */
    DEMO_STORAGE_SOURCE *demo_ss; /* Enclosing storage source */

    TAILQ_ENTRY(demo_file_handle) q; /* Queue of handles */
    uint32_t ref;                    /* Reference count */

    char *buf;      /* In-memory contents */
    size_t bufsize; /* In-memory buffer size */

    size_t size; /* Read/write data size */
} DEMO_FILE_HANDLE;

typedef struct demo_location_handle {
    WT_LOCATION_HANDLE iface;

    char *loc_string; /* location as a string. */
} DEMO_LOCATION_HANDLE;

#define LOCATION_STRING(lh) (((DEMO_LOCATION_HANDLE *)lh)->loc_string)

/*
 * Extension initialization function.
 */
#ifdef _WIN32
/*
 * Explicitly export this function so it is visible when loading extensions.
 */
__declspec(dllexport)
#endif
  int demo_storage_source_create(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * Forward function declarations for storage source API implementation.
 */
static int demo_ss_exist(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_LOCATION_HANDLE *, const char *, bool *);
static int demo_ss_location_handle(
  WT_STORAGE_SOURCE *, WT_SESSION *, const char *, WT_LOCATION_HANDLE **);
static int demo_ss_location_list(WT_STORAGE_SOURCE *, WT_SESSION *, WT_LOCATION_HANDLE *,
  const char *, uint32_t, char ***, uint32_t *);
static int demo_ss_location_list_free(WT_STORAGE_SOURCE *, WT_SESSION *, char **, uint32_t);
static int demo_ss_open(WT_STORAGE_SOURCE *, WT_SESSION *, WT_LOCATION_HANDLE *, const char *,
  uint32_t, WT_FILE_HANDLE **);
static int demo_ss_remove(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_LOCATION_HANDLE *, const char *, uint32_t);
static int demo_ss_size(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_LOCATION_HANDLE *, const char *, wt_off_t *);
static int demo_ss_terminate(WT_STORAGE_SOURCE *, WT_SESSION *);

/*
 * Forward function declarations for location API implementation.
 */
static int demo_location_close(WT_LOCATION_HANDLE *, WT_SESSION *);

/*
 * Forward function declarations for file handle API implementation.
 */
static int demo_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int demo_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int demo_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int demo_file_sync(WT_FILE_HANDLE *, WT_SESSION *);
static int demo_file_truncate(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t);
static int demo_file_write(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, const void *);

/*
 * Forward function declarations for internal functions.
 */
static int demo_handle_remove(WT_SESSION *, DEMO_FILE_HANDLE *);
static DEMO_FILE_HANDLE *demo_handle_search(
  WT_STORAGE_SOURCE *, WT_LOCATION_HANDLE *, const char *);

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
 * demo_storage_source_create --
 *     Initialize the demo storage source.
 */
int
demo_storage_source_create(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_CONFIG_ITEM k, v;
    WT_CONFIG_PARSER *config_parser;
    WT_EXTENSION_API *wtext;
    WT_STORAGE_SOURCE *storage_source;
    int ret = 0;

    wtext = conn->get_extension_api(conn);

    if ((demo_ss = calloc(1, sizeof(DEMO_STORAGE_SOURCE))) == NULL) {
        (void)wtext->err_printf(
          wtext, NULL, "demo_storage_source_create: %s", wtext->strerror(wtext, NULL, ENOMEM));
        return (ENOMEM);
    }
    demo_ss->wtext = wtext;
    storage_source = (WT_STORAGE_SOURCE *)demo_ss;

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
    printf("Custom storage source configuration\n");
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

    allocate_storage_source_lock(&demo_ss->lock);

    /* Initialize the in-memory jump table. */
    storage_source->ss_exist = demo_ss_exist;
    storage_source->ss_location_handle = demo_ss_location_handle;
    storage_source->ss_location_list = demo_ss_location_list;
    storage_source->ss_location_list_free = demo_ss_location_list_free;
    storage_source->ss_open_object = demo_ss_open;
    storage_source->ss_remove = demo_ss_remove;
    storage_source->ss_size = demo_ss_size;
    storage_source->terminate = demo_ss_terminate;

    if ((ret = conn->add_storage_source(conn, "demo", storage_source, NULL)) != 0) {
        (void)wtext->err_printf(
          wtext, NULL, "WT_CONNECTION.set_storage_source: %s", wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    return (0);

err:
    free(demo_ss);
    /* An error installing the storage source is fatal. */
    exit(1);
}

/*
 * demo_ss_open --
 *     fopen for our demo storage source.
 */
static int
demo_ss_open(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_LOCATION_HANDLE *location_handle, const char *name, uint32_t flags,
  WT_FILE_HANDLE **file_handlep)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;
    WT_FILE_HANDLE *file_handle;
    const char *location;
    char *full_name;
    size_t name_len;
    int ret = 0;

    (void)flags; /* Unused */

    *file_handlep = NULL;

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;
    demo_fh = NULL;
    wtext = demo_ss->wtext;

    lock_storage_source(&demo_ss->lock);
    ++demo_ss->opened_object_count;

    /*
     * First search the file queue, if we find it, assert there's only a single reference, we only
     * support a single handle on any file.
     */
    demo_fh = demo_handle_search(storage_source, location_handle, name);
    if (demo_fh != NULL) {
        if (demo_fh->ref != 0) {
            (void)wtext->err_printf(wtext, session, "demo_ss_open: %s: file already open", name);
            ret = EBUSY;
            goto err;
        }

        demo_fh->ref = 1;
        *file_handlep = (WT_FILE_HANDLE *)demo_fh;
        unlock_storage_source(&demo_ss->lock);
        return (0);
    }

    /* The file hasn't been opened before, create a new one. */
    if ((demo_fh = calloc(1, sizeof(DEMO_FILE_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    /* Initialize private information. */
    demo_fh->demo_ss = demo_ss;
    demo_fh->ref = 1;
    if ((demo_fh->buf = calloc(1, DEMO_FILE_SIZE_INCREMENT)) == NULL) {
        ret = ENOMEM;
        goto err;
    }
    demo_fh->bufsize = DEMO_FILE_SIZE_INCREMENT;
    demo_fh->size = 0;

    /* Construct the public name. */
    location = LOCATION_STRING(location_handle);
    name_len = strlen(location) + strlen(name) + 1;
    full_name = calloc(1, name_len);
    if (snprintf(full_name, name_len, "%s%s", location, name) != (ssize_t)(name_len - 1)) {
        ret = ENOMEM;
        goto err;
    }

    /* Initialize public information. */
    file_handle = (WT_FILE_HANDLE *)demo_fh;
    file_handle->name = full_name;

    /*
     * Setup the function call table for our custom storage source. Set the function pointer to NULL
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
    file_handle->fh_read = demo_file_read;
    file_handle->fh_size = demo_file_size;
    file_handle->fh_sync = demo_file_sync;
    file_handle->fh_sync_nowait = NULL;
    file_handle->fh_truncate = demo_file_truncate;
    file_handle->fh_unmap = NULL;
    file_handle->fh_write = demo_file_write;

    TAILQ_INSERT_HEAD(&demo_ss->fileq, demo_fh, q);
    ++demo_ss->opened_unique_object_count;

    *file_handlep = file_handle;

    if (0) {
err:
        free(demo_fh->buf);
        free(demo_fh);
    }

    unlock_storage_source(&demo_ss->lock);
    return (ret);
}

/*
 * demo_ss_location_handle --
 *     Return a location handle from a location string.
 */
static int
demo_ss_location_handle(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *location_info, WT_LOCATION_HANDLE **location_handlep)
{
    DEMO_LOCATION_HANDLE *demo_loc;
    size_t len;
    int ret;
    char *p;

    (void)storage_source; /* Unused */
    (void)session;        /* Unused */

    ret = 0;
    p = NULL;
    demo_loc = NULL;

    /*
     * We save the location string we're given followed by a slash delimiter. We won't allow slashes
     * in the location info parameter.
     */
    if (strchr(location_info, '/') != NULL)
        return (EINVAL);
    len = strlen(location_info) + 2;
    p = malloc(len);
    if (snprintf(p, len, "%s/", location_info) != (ssize_t)(len - 1)) {
        ret = ENOMEM;
        goto err;
    }

    /*
     * Now create the location handle and save the string.
     */
    if ((demo_loc = calloc(1, sizeof(DEMO_LOCATION_HANDLE))) == NULL) {
        ret = ENOMEM;
        goto err;
    }

    /* Initialize private information. */
    demo_loc->loc_string = p;

    /* Initialize public information. */
    demo_loc->iface.close = demo_location_close;

    *location_handlep = &demo_loc->iface;

err:
    if (ret != 0) {
        free(p);
        free(demo_loc);
        return (ret);
    }
    return (0);
}

/*
 * demo_ss_location_list --
 *     Return a list of object names for the given location.
 */
static int
demo_ss_location_list(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_LOCATION_HANDLE *location_handle, const char *prefix, uint32_t limit, char ***dirlistp,
  uint32_t *countp)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_STORAGE_SOURCE *demo_ss;
    size_t location_len, prefix_len;
    uint32_t allocated, count;
    int ret = 0;
    const char *location;
    char **entries, *name;
    void *p;

    (void)session; /* Unused */

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;

    *dirlistp = NULL;
    *countp = 0;

    entries = NULL;
    allocated = count = 0;
    location = LOCATION_STRING(location_handle);
    location_len = strlen(location);
    prefix_len = (prefix == NULL ? 0 : strlen(prefix));

    lock_storage_source(&demo_ss->lock);
    TAILQ_FOREACH (demo_fh, &demo_ss->fileq, q) {
        name = demo_fh->iface.name;
        if (strncmp(name, location, location_len) != 0)
            continue;
        name += location_len;
        if (prefix != NULL && strncmp(name, prefix, prefix_len) != 0)
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
        if (limit > 0 && count >= limit)
            break;
    }

    *dirlistp = entries;
    *countp = count;

err:
    unlock_storage_source(&demo_ss->lock);
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
 * demo_ss_location_list_free --
 *     Free memory allocated by demo_ss_location_list.
 */
static int
demo_ss_location_list_free(
  WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, char **dirlist, uint32_t count)
{
    (void)storage_source;
    (void)session;

    if (dirlist != NULL) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }
    return (0);
}

/*
 * demo_ss_exist --
 *     Return if the file exists.
 */
static int
demo_ss_exist(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_LOCATION_HANDLE *location_handle, const char *name, bool *existp)
{
    DEMO_STORAGE_SOURCE *demo_ss;

    (void)session; /* Unused */

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;

    lock_storage_source(&demo_ss->lock);
    *existp = demo_handle_search(storage_source, location_handle, name) != NULL;
    unlock_storage_source(&demo_ss->lock);

    return (0);
}

/*
 * demo_ss_remove --
 *     POSIX remove.
 */
static int
demo_ss_remove(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_LOCATION_HANDLE *location_handle, const char *name, uint32_t flags)
{
    DEMO_STORAGE_SOURCE *demo_ss;
    DEMO_FILE_HANDLE *demo_fh;
    int ret = 0;

    (void)session; /* Unused */
    (void)flags;   /* Unused */

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;

    ret = ENOENT;
    lock_storage_source(&demo_ss->lock);
    if ((demo_fh = demo_handle_search(storage_source, location_handle, name)) != NULL)
        ret = demo_handle_remove(session, demo_fh);
    unlock_storage_source(&demo_ss->lock);

    return (ret);
}

/*
 * demo_ss_size --
 *     Get the size of a file in bytes, by file name.
 */
static int
demo_ss_size(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_LOCATION_HANDLE *location_handle, const char *name, wt_off_t *sizep)
{
    DEMO_STORAGE_SOURCE *demo_ss;
    DEMO_FILE_HANDLE *demo_fh;
    int ret = 0;

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;

    ret = ENOENT;
    lock_storage_source(&demo_ss->lock);
    if ((demo_fh = demo_handle_search(storage_source, location_handle, name)) != NULL)
        ret = demo_file_size((WT_FILE_HANDLE *)demo_fh, session, sizep);
    unlock_storage_source(&demo_ss->lock);

    return (ret);
}

/*
 * demo_ss_terminate --
 *     Discard any resources on termination.
 */
static int
demo_ss_terminate(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session)
{
    DEMO_FILE_HANDLE *demo_fh, *demo_fh_tmp;
    DEMO_STORAGE_SOURCE *demo_ss;
    int ret = 0, tret;

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;

    TAILQ_FOREACH_SAFE(demo_fh, &demo_ss->fileq, q, demo_fh_tmp)
    if ((tret = demo_handle_remove(session, demo_fh)) != 0 && ret == 0)
        ret = tret;

    printf("Custom storage source\n");
    printf("\t%d unique object opens\n", demo_ss->opened_unique_object_count);
    printf("\t%d objects opened\n", demo_ss->opened_object_count);
    printf("\t%d objects closed\n", demo_ss->closed_object_count);
    printf("\t%d reads, %d writes\n", demo_ss->read_ops, demo_ss->write_ops);

    destroy_storage_source_lock(&demo_ss->lock);
    free(demo_ss);

    return (ret);
}

/*
 * demo_location_close --
 *     Free a location handle created by ss_location_handle.
 */
static int
demo_location_close(WT_LOCATION_HANDLE *location_handle, WT_SESSION *session)
{
    (void)session; /* Unused */

    free(LOCATION_STRING(location_handle));
    free(location_handle);
    return (0);
}

/*
 * demo_file_close --
 *     ANSI C close.
 */
static int
demo_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_STORAGE_SOURCE *demo_ss;

    (void)session; /* Unused */

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_ss = demo_fh->demo_ss;

    lock_storage_source(&demo_ss->lock);
    if (--demo_fh->ref == 0)
        ++demo_ss->closed_object_count;
    unlock_storage_source(&demo_ss->lock);

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
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;
    size_t off;
    int ret = 0;

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_ss = demo_fh->demo_ss;
    wtext = demo_ss->wtext;
    off = (size_t)offset;

    lock_storage_source(&demo_ss->lock);
    ++demo_ss->read_ops;
    if (off < demo_fh->size) {
        if (len > demo_fh->size - off)
            len = demo_fh->size - off;
        memcpy(buf, (uint8_t *)demo_fh->buf + off, len);
    } else
        ret = EIO; /* EOF */
    unlock_storage_source(&demo_ss->lock);
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
    DEMO_STORAGE_SOURCE *demo_ss;

    (void)session; /* Unused */

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_ss = demo_fh->demo_ss;

    lock_storage_source(&demo_ss->lock);
    *sizep = (wt_off_t)demo_fh->size;
    unlock_storage_source(&demo_ss->lock);
    return (0);
}

/*
 * demo_file_sync --
 *     Ensure the content of the file is stable. This is a no-op in our memory backed storage
 *     source.
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
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;
    size_t off;
    void *p;

    demo_ss = demo_fh->demo_ss;
    wtext = demo_ss->wtext;
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
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;

    (void)file_handle; /* Unused */
    (void)session;     /* Unused */
    (void)offset;      /* Unused */

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_ss = demo_fh->demo_ss;
    wtext = demo_ss->wtext;

    (void)wtext->err_printf(wtext, session, "%s: truncate not supported in storage source",
      demo_fh->iface.name, wtext->strerror(wtext, NULL, ENOTSUP));
    return (ENOTSUP);
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
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;
    size_t off;
    int ret = 0;

    demo_fh = (DEMO_FILE_HANDLE *)file_handle;
    demo_ss = demo_fh->demo_ss;
    wtext = demo_ss->wtext;
    off = (size_t)offset;

    lock_storage_source(&demo_ss->lock);
    ++demo_ss->write_ops;
    if ((ret = demo_buffer_resize(
           session, demo_fh, offset + (wt_off_t)(len + DEMO_FILE_SIZE_INCREMENT))) == 0) {
        memcpy((uint8_t *)demo_fh->buf + off, buf, len);
        if (off + len > demo_fh->size)
            demo_fh->size = off + len;
    }
    unlock_storage_source(&demo_ss->lock);
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
    DEMO_STORAGE_SOURCE *demo_ss;
    WT_EXTENSION_API *wtext;

    demo_ss = demo_fh->demo_ss;
    wtext = demo_ss->wtext;

    if (demo_fh->ref != 0) {
        (void)wtext->err_printf(wtext, session, "demo_handle_remove: %s: file is currently open",
          demo_fh->iface.name, wtext->strerror(wtext, NULL, EBUSY));
        return (EBUSY);
    }

    TAILQ_REMOVE(&demo_ss->fileq, demo_fh, q);

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
demo_handle_search(
  WT_STORAGE_SOURCE *storage_source, WT_LOCATION_HANDLE *location_handle, const char *name)
{
    DEMO_FILE_HANDLE *demo_fh;
    DEMO_STORAGE_SOURCE *demo_ss;
    size_t len;
    const char *location;

    demo_ss = (DEMO_STORAGE_SOURCE *)storage_source;
    location = LOCATION_STRING(location_handle);
    len = strlen(location);

    TAILQ_FOREACH (demo_fh, &demo_ss->fileq, q)
        if (strncmp(demo_fh->iface.name, location, len) == 0 &&
          strcmp(&demo_fh->iface.name[len], name) == 0)
            break;
    return (demo_fh);
}

static const char *home;

static int
demo_test_create(WT_STORAGE_SOURCE *ss, WT_SESSION *session, WT_LOCATION_HANDLE *location,
  const char *objname, const char *content)
{
    WT_FILE_HANDLE *fh;
    const char *op;
    size_t len;
    int ret, t_ret;

    fh = NULL;
    len = strlen(content) + 1;
    op = "open";
    if ((ret = ss->ss_open_object(ss, session, location, objname, WT_SS_OPEN_CREATE, &fh)) != 0)
        goto err;
    op = "write";
    if ((ret = fh->fh_write(fh, session, 0, len, content)) != 0)
        goto err;

err:
    if (fh != NULL && (t_ret = fh->close(fh, session)) != 0 && ret == 0) {
        op = "close";
        ret = t_ret;
    }
    if (ret != 0)
        fprintf(stderr, "demo failed during %s: %s\n", op, wiredtiger_strerror(ret));
    else
        printf("demo succeeded create %s\n", objname);

    return (ret);
}

static int
demo_test_read(WT_STORAGE_SOURCE *ss, WT_SESSION *session, WT_LOCATION_HANDLE *location,
  const char *objname, const char *content)
{
    WT_FILE_HANDLE *fh;
    char buf[100];
    const char *op;
    size_t len;
    wt_off_t size;
    int ret, t_ret;

    fh = NULL;
    len = strlen(content) + 1;

    /* Set the op string so that on error we know what failed. */
    op = "open";
    if ((ret = ss->ss_open_object(ss, session, location, objname, WT_SS_OPEN_READONLY, &fh)) != 0)
        goto err;
    op = "size";
    if ((ret = fh->fh_size(fh, session, &size)) != 0)
        goto err;
    op = "size-compare";
    if ((size_t)size != len || (size_t)size > sizeof(buf)) {
        ret = EINVAL;
        goto err;
    }
    op = "read";
    if ((ret = fh->fh_read(fh, session, 0, len, buf)) != 0)
        goto err;
    op = "read-compare";
    if (strncmp(buf, content, len) != 0) {
        ret = EINVAL;
        goto err;
    }

err:
    if (fh != NULL && (t_ret = fh->close(fh, session)) != 0 && ret == 0) {
        op = "close";
        ret = t_ret;
    }
    if (ret != 0)
        fprintf(stderr, "demo failed during %s: %s\n", op, wiredtiger_strerror(ret));
    else
        printf("demo succeeded read %s\n", objname);

    return (ret);
}

static int
demo_test_list(WT_STORAGE_SOURCE *ss, WT_SESSION *session, const char *description,
  WT_LOCATION_HANDLE *location, const char *prefix, uint32_t limit, uint32_t expect)
{
    char **obj_list;
    const char *op;
    uint32_t i, obj_count;
    int ret, t_ret;

    obj_list = NULL;
    /* Set the op string so that on error we know what failed. */
    op = "location_list";
    if ((ret = ss->ss_location_list(ss, session, location, prefix, limit, &obj_list, &obj_count)) !=
      0)
        goto err;
    op = "location_list count";
    if (obj_count != expect) {
        ret = EINVAL;
        goto err;
    }
    printf("list: %s:\n", description);
    for (i = 0; i < obj_count; i++) {
        printf("  %s\n", obj_list[i]);
    }

err:
    if (obj_list != NULL &&
      (t_ret = ss->ss_location_list_free(ss, session, obj_list, obj_count)) != 0 && ret == 0) {
        op = "location_list_free";
        ret = t_ret;
    }
    if (ret != 0)
        fprintf(stderr, "demo failed during %s: %s\n", op, wiredtiger_strerror(ret));
    else
        printf("demo succeeded location_list %s\n", description);

    return (ret);
}

static int
demo_test_storage_source(WT_STORAGE_SOURCE *ss, WT_SESSION *session)
{
    WT_LOCATION_HANDLE *location1, *location2;
    const char *op;
    int ret, t_ret;
    bool exist;

    location1 = location2 = NULL;

    /* Create two locations. Set the op string so that on error we know what failed. */
    op = "location_handle";
    if ((ret = ss->ss_location_handle(ss, session, "location-one", &location1)) != 0)
        goto err;
    if ((ret = ss->ss_location_handle(ss, session, "location-two", &location2)) != 0)
        goto err;

    /*
     * Create and existence checks. In location-one, create "A". In location-two, create "A", "B",
     * "AA". We'll do simple lists of both locations, and a list of location-two with a prefix.
     */
    op = "create/exist checks";
    if ((ret = demo_test_create(ss, session, location1, "A", "location-one-A")) != 0)
        goto err;

    if ((ret = ss->ss_exist(ss, session, location1, "A", &exist)) != 0)
        goto err;
    if (!exist) {
        fprintf(stderr, "Exist test failed for A\n");
        ret = EINVAL;
        goto err;
    }
    if ((ret = ss->ss_exist(ss, session, location2, "A", &exist)) != 0)
        goto err;
    if (exist) {
        fprintf(stderr, "Exist test failed for A in location2\n");
        ret = EINVAL;
        goto err;
    }

    if ((ret = demo_test_create(ss, session, location2, "A", "location-two-A")) != 0)
        goto err;
    if ((ret = demo_test_create(ss, session, location2, "B", "location-two-B")) != 0)
        goto err;
    if ((ret = demo_test_create(ss, session, location2, "AA", "location-two-AA")) != 0)
        goto err;

    /* Make sure the objects contain the expected data. */
    op = "read checks";
    if ((ret = demo_test_read(ss, session, location1, "A", "location-one-A")) != 0)
        goto err;
    if ((ret = demo_test_read(ss, session, location2, "A", "location-two-A")) != 0)
        goto err;
    if ((ret = demo_test_read(ss, session, location2, "B", "location-two-B")) != 0)
        goto err;

    /*
     * List the locations. For location-one, we expect just one object.
     */
    op = "list checks";
    if ((ret = demo_test_list(ss, session, "location1", location1, NULL, 0, 1)) != 0)
        goto err;

    /*
     * For location-two, we expect three objects.
     */
    if ((ret = demo_test_list(ss, session, "location2", location2, NULL, 0, 3)) != 0)
        goto err;

    /*
     * If we limit the number of objects received to 2, we should only see 2.
     */
    if ((ret = demo_test_list(ss, session, "location2, limit:2", location2, NULL, 2, 2)) != 0)
        goto err;

    /*
     * With a prefix of "A", and no limit, we'll see two objects.
     */
    if ((ret = demo_test_list(ss, session, "location2: A", location2, "A", 0, 2)) != 0)
        goto err;

    /*
     * With a prefix of "A", and a limit of one, we'll see just one object.
     */
    if ((ret = demo_test_list(ss, session, "location2: A, limit:1", location2, "A", 1, 1)) != 0)
        goto err;

err:
    if (location1 != NULL && (t_ret = location1->close(location1, session)) != 0 && ret == 0)
        ret = t_ret;
    if (location2 != NULL && (t_ret = location2->close(location2, session)) != 0 && ret == 0)
        ret = t_ret;
    if (ret != 0)
        fprintf(stderr, "demo failed during %s: %s\n", op, wiredtiger_strerror(ret));

    return (ret);
}

int
main(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_STORAGE_SOURCE *storage_source;
    const char *open_config;
    int ret;

    fprintf(stderr, "ex_storage_source: starting\n");
    /*
     * Create a clean test directory for this run of the test program if the environment variable
     * isn't already set (as is done by make check).
     */
    if (getenv("WIREDTIGER_HOME") == NULL) {
        home = "WT_HOME";
        if ((ret = system("rm -rf WT_HOME && mkdir WT_HOME")) != 0) {
            fprintf(stderr, "system: directory recreate failed: %s\n", strerror(ret));
            return (EXIT_FAILURE);
        }
    } else
        home = NULL;

    /*! [WT_STORAGE_SOURCE register] */
    /*
     * Setup a configuration string that will load our custom storage source. Use the special local
     * extension to indicate that the entry point is in the same executable. Finally, pass in two
     * pieces of configuration information to our initialization function as the "config" value.
     */
    open_config =
      "create,log=(enabled=true),extensions=(local={entry=demo_storage_source_create,"
      "config={config_string=\"demo-storage-source\",config_value=37}})";
    /* Open a connection to the database, creating it if necessary. */
    if ((ret = wiredtiger_open(home, NULL, open_config, &conn)) != 0) {
        fprintf(stderr, "Error connecting to %s: %s\n", home == NULL ? "." : home,
          wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    /*! [WT_STORAGE_SOURCE register] */

    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
        fprintf(stderr, "WT_CONNECTION.open_session: %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    if ((ret = conn->get_storage_source(conn, "demo", &storage_source)) != 0) {
        fprintf(stderr, "WT_CONNECTION.get_storage_source: %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    /*
     * At the moment, the infrastructure within WiredTiger that would use the storage source
     * extension does not exist. So call the interface directly as a demonstration.
     */
    if ((ret = demo_test_storage_source(storage_source, session)) != 0) {
        fprintf(stderr, "storage source test failed: %s\n", wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }
    if ((ret = conn->close(conn, NULL)) != 0) {
        fprintf(stderr, "Error closing connection to %s: %s\n", home == NULL ? "." : home,
          wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    return (EXIT_SUCCESS);
}
