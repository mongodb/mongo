#include "wiredtiger.h"
#include "wiredtiger_ext.h"
#include <fstream>
#include <list>
#include <errno.h>
#include <filesystem>
#include <mutex>

#include "gcp_connection.h"
#include "wt_internal.h"

struct gcp_file_system;
struct gcp_file_handle;

/*
 * The first struct member must be the WT interface that is being implemented.
 */
struct gcp_store {
    WT_STORAGE_SOURCE store;
    std::vector<gcp_file_system> gcp_fs;
};

struct gcp_file_system {
    WT_FILE_SYSTEM fs;
    gcp_store *store;
    std::vector<gcp_file_handle> gcp_fh;
    gcp_connection *gcp_conn;
};

struct gcp_file_handle {
    WT_FILE_HANDLE fh;
    gcp_store *store;
};

static int gcp_customize_file_system(WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *,
  const char *, WT_FILE_SYSTEM **) __attribute__((__unused__));
static int gcp_add_reference(WT_STORAGE_SOURCE *) __attribute__((__unused__));
static int gcp_file_system_terminate(WT_FILE_SYSTEM *, WT_SESSION *) __attribute__((__unused__));
static int gcp_flush(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));
static int gcp_flush_finish(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));
static int gcp_file_exists(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *)
  __attribute__((__unused__));
static int gcp_file_open(WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE,
  uint32_t, WT_FILE_HANDLE **) __attribute__((__unused__));
static int gcp_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t)
  __attribute__((__unused__));
static int gcp_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t)
  __attribute__((__unused__));
static int gcp_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *) __attribute__((__unused__));
static int gcp_object_list_add(const gcp_store &, char ***, const std::vector<std::string> &,
  const uint32_t) __attribute__((__unused__));
static int gcp_object_list_single(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *,
  char ***, uint32_t *) __attribute__((__unused__));
static int gcp_object_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t)
  __attribute__((__unused__));

static int
gcp_customize_file_system(WT_STORAGE_SOURCE *store, WT_SESSION *session, const char *bucket,
  const char *auth_token, const char *config, WT_FILE_SYSTEM **file_system)
{
    WT_UNUSED(store);
    WT_UNUSED(session);
    WT_UNUSED(bucket);
    WT_UNUSED(auth_token);
    WT_UNUSED(config);
    WT_UNUSED(file_system);

    return 0;
}

static int
gcp_add_reference(WT_STORAGE_SOURCE *store)
{
    WT_UNUSED(store);

    return 0;
}

static int
gcp_file_system_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);

    return 0;
}

static int
gcp_flush(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
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
gcp_flush_finish(WT_STORAGE_SOURCE *storage, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
  const char *source, const char *object, const char *config)
{
    WT_UNUSED(storage);
    WT_UNUSED(session);
    WT_UNUSED(file_system);
    WT_UNUSED(source);
    WT_UNUSED(object);
    WT_UNUSED(config);

    return 0;
}

static int
gcp_file_exists(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *file_exists)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(file_exists);

    return 0;
}

static int
gcp_file_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handle_ptr)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(file_type);
    WT_UNUSED(flags);
    WT_UNUSED(file_handle_ptr);

    return 0;
}

static int
gcp_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(flags);

    return 0;
}

static int
gcp_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
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
gcp_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    WT_UNUSED(file_handle);
    WT_UNUSED(session);
    WT_UNUSED(sizep);

    return 0;
}

static int
gcp_object_list_add(const gcp_store &gcp_, char ***object_list,
  const std::vector<std::string> &objects, const uint32_t count)
{
    WT_UNUSED(gcp_);
    WT_UNUSED(object_list);
    WT_UNUSED(objects);
    WT_UNUSED(count);

    return 0;
}

static int
gcp_object_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***object_list, uint32_t *count)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(directory);
    WT_UNUSED(prefix);
    WT_UNUSED(object_list);
    WT_UNUSED(count);

    return 0;
}

static int
gcp_object_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **object_list, uint32_t count)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(object_list);
    WT_UNUSED(count);

    return 0;
}

int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    WT_UNUSED(connection);
    WT_UNUSED(config);

    return 0;
}
