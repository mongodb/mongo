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
#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include <vector>

#include "azure_connection.h"
#include "wt_internal.h"

struct azure_file_system;
struct azure_file_handle;
struct azure_store {
    WT_STORAGE_SOURCE store;
    std::vector<azure_file_system *> azure_fs;
};

struct azure_file_system {
    WT_FILE_SYSTEM fs;
    azure_store *store;
    std::vector<azure_file_handle> azure_fh;
    azure_connection *azure_conn;
};

struct azure_file_handle {
    WT_FILE_HANDLE fh;
    azure_store *store;
};

// WT_STORAGE_SOURCE Interface
static int azure_customize_file_system(WT_STORAGE_SOURCE *, WT_SESSION *, const char *,
  const char *, const char *, WT_FILE_SYSTEM **) __attribute__((__unused__));
static int azure_add_reference(WT_STORAGE_SOURCE *) __attribute__((__unused__));
static int azure_terminate(WT_FILE_SYSTEM *, WT_SESSION *) __attribute__((__unused__));
static int azure_flush(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));
static int azure_flush_finish(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));

// WT_FILE_SYSTEM Interface
static int azure_object_list(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***,
  uint32_t *) __attribute__((__unused__));
static int azure_object_list_single(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *,
  char ***, uint32_t *) __attribute__((__unused__));
static int azure_object_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t)
  __attribute__((__unused__));
static int azure_file_system_terminate(WT_FILE_SYSTEM *, WT_SESSION *) __attribute__((__unused__));
static int azure_file_exists(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *)
  __attribute__((__unused__));
static int azure_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t)
  __attribute__((__unused__));
static int azure_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t)
  __attribute__((__unused__));
static int azure_object_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *)
  __attribute__((__unused__));
static int azure_file_open(WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE,
  uint32_t, WT_FILE_HANDLE **) __attribute__((__unused__));

// WT_FILE_HANDLE Interface
static int azure_file_close(WT_FILE_HANDLE *, WT_SESSION *) __attribute__((__unused__));
static int azure_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool) __attribute__((__unused__));
static int azure_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *)
  __attribute__((__unused__));
static int azure_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *) __attribute__((__unused__));

static int
azure_customize_file_system(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *bucket_name, const char *auth_token, const char *config,
  WT_FILE_SYSTEM **file_systemp)
{
    WT_UNUSED(storage_source);
    WT_UNUSED(session);
    WT_UNUSED(bucket_name);
    WT_UNUSED(auth_token);
    WT_UNUSED(config);
    WT_UNUSED(file_systemp);

    return 0;
}

static int
azure_add_reference(WT_STORAGE_SOURCE *storage_source)
{
    WT_UNUSED(storage_source);

    return 0;
}

static int
azure_flush(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
  const char *source, const char *object, const char *config)
{
    WT_UNUSED(storage_source);
    WT_UNUSED(session);
    WT_UNUSED(file_system);
    WT_UNUSED(source);
    WT_UNUSED(object);
    WT_UNUSED(config);

    return 0;
}

static int
azure_flush_finish(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_FILE_SYSTEM *file_system, const char *source, const char *object, const char *config)
{
    WT_UNUSED(storage_source);
    WT_UNUSED(session);
    WT_UNUSED(file_system);
    WT_UNUSED(source);
    WT_UNUSED(object);
    WT_UNUSED(config);

    return 0;
}

static int
azure_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);

    return 0;
}

static int
azure_object_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(directory);
    WT_UNUSED(prefix);
    WT_UNUSED(dirlistp);
    WT_UNUSED(countp);

    return 0;
}

static int
azure_object_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(directory);
    WT_UNUSED(prefix);
    WT_UNUSED(dirlistp);
    WT_UNUSED(countp);

    return 0;
}

static int
azure_object_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(dirlist);
    WT_UNUSED(count);

    return 0;
}

static int
azure_file_system_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);

    return 0;
}

static int
azure_file_exists(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(existp);

    return 0;
}

static int
azure_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(flags);

    return 0;
}

static int
azure_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(from);
    WT_UNUSED(to);
    WT_UNUSED(flags);

    return 0;
}

static int
azure_object_size(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(sizep);

    return 0;
}

static int
azure_file_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(file_type);
    WT_UNUSED(flags);
    WT_UNUSED(file_handlep);

    return 0;
}

static int
azure_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    WT_UNUSED(file_handle);
    WT_UNUSED(session);

    return 0;
}

static int
azure_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    WT_UNUSED(file_handle);
    WT_UNUSED(session);
    WT_UNUSED(lock);

    return 0;
}

static int
azure_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    WT_UNUSED(file_handle);
    WT_UNUSED(session);
    WT_UNUSED(offset);
    WT_UNUSED(len);
    WT_UNUSED(buf);

    return 0;
}

static int
azure_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    WT_UNUSED(file_handle);
    WT_UNUSED(session);
    WT_UNUSED(sizep);

    return 0;
}

int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    WT_UNUSED(connection);
    WT_UNUSED(config);

    return 0;
}
