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
 * The first struct member must be the WiredTiger interface that is being implemented.
 */
struct gcp_store {
    WT_STORAGE_SOURCE storage_source;
    WT_EXTENSION_API *wt_api;
    std::mutex fs_list_mutex;
    std::vector<gcp_file_system *> fs_list;
    uint32_t reference_count;
    WT_VERBOSE_LEVEL verbose;
};

struct gcp_file_system {
    WT_FILE_SYSTEM file_system;
    gcp_store *storage_source;
    WT_FILE_SYSTEM *wt_file_system;
    std::unique_ptr<gcp_connection> gcp_conn;
    std::mutex fh_list_mutex;
    std::vector<gcp_file_handle *> fh_list;
    std::string home_dir;
};

struct gcp_file_handle {
    WT_FILE_HANDLE fh;
    gcp_file_system *file_system;
    std::string name;
    uint32_t reference_count;
};

static gcp_file_system *get_gcp_file_system(WT_FILE_SYSTEM *);
static int gcp_customize_file_system(WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *,
  const char *, WT_FILE_SYSTEM **) __attribute__((__unused__));
static int gcp_add_reference(WT_STORAGE_SOURCE *) __attribute__((__unused__));
static int gcp_file_system_terminate(WT_FILE_SYSTEM *, WT_SESSION *) __attribute__((__unused__));
static int gcp_flush(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));
static int gcp_flush_finish(WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *,
  const char *, const char *) __attribute__((__unused__));
static int gcp_file_close(WT_FILE_HANDLE *, WT_SESSION *) __attribute__((__unused__));
static int gcp_file_exists(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *)
  __attribute__((__unused__));
static int gcp_file_open(WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE,
  uint32_t, WT_FILE_HANDLE **) __attribute__((__unused__));
static int gcp_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t)
  __attribute__((__unused__));
static int gcp_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t)
  __attribute__((__unused__));
static int gcp_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool) __attribute__((__unused__));
static int gcp_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *) __attribute__((__unused__));
static int gcp_object_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *)
  __attribute__((__unused__));
static int gcp_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int gcp_object_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int gcp_object_list_add(const gcp_store &, char ***, const std::vector<std::string> &,
  const uint32_t) __attribute__((__unused__));
static int gcp_object_list_single(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *,
  char ***, uint32_t *) __attribute__((__unused__));
static int gcp_object_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t)
  __attribute__((__unused__));
static int gcp_terminate(WT_STORAGE_SOURCE *, WT_SESSION *) __attribute__((__unused__));

static gcp_file_system *
get_gcp_file_system(WT_FILE_SYSTEM *file_system)
{
    return reinterpret_cast<gcp_file_system *>(file_system);
}

static int
gcp_customize_file_system(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *bucket, const char *auth_file, const char *config, WT_FILE_SYSTEM **file_system)
{
    // Check if bucket name is given
    if (bucket == nullptr || strlen(bucket) == 0) {
        std::cerr << "gcp_customize_file_system: bucket not specified." << std::endl;
        return EINVAL;
    }

    // Fail if there is no authentication provided.
    if (auth_file == nullptr || strlen(auth_file) == 0) {
        std::cerr << "gcp_customize_file_system: auth_file not specified." << std::endl;
        return EINVAL;
    }

    if (std::filesystem::path(auth_file).extension() != ".json") {
        std::cerr << "gcp_customize_file_system: improper auth_file: " + std::string(auth_file) +
            " should be a .json file."
                  << std::endl;
        return EINVAL;
    }

    gcp_store *gcp = reinterpret_cast<gcp_store *>(storage_source);
    int ret;

    // Get any prefix to be used for the object keys.
    WT_CONFIG_ITEM obj_prefix_conf;
    std::string obj_prefix;
    if ((ret = gcp->wt_api->config_get_string(
           gcp->wt_api, session, config, "prefix", &obj_prefix_conf)) == 0)
        obj_prefix = std::string(obj_prefix_conf.str, obj_prefix_conf.len);
    else if (ret != WT_NOTFOUND) {
        std::cerr << "gcp_customize_file_system: error parsing config for object prefix."
                  << std::endl;
        return ret;
    }

    // Fetch the native WiredTiger file system.
    WT_FILE_SYSTEM *wt_file_system;
    if ((ret = gcp->wt_api->file_system_get(gcp->wt_api, session, &wt_file_system)) != 0) {
        std::cerr << "gcp_customize_file_system: failed to fetch the native WireTiger file system"
                  << std::endl;
        return ret;
    }

    // Create the file system and allocate memory for file system.
    gcp_file_system *fs;
    try {
        fs = new gcp_file_system;
    } catch (std::bad_alloc &e) {
        std::cerr << "gcp_customize_file_system: " << e.what() << std::endl;
        return ENOMEM;
    }

    // Set variables specific to GCP.
    fs->storage_source = gcp;
    fs->wt_file_system = wt_file_system;
    fs->home_dir = session->connection->get_home(session->connection);

    // Create connection to google cloud.
    try {
        fs->gcp_conn = std::make_unique<gcp_connection>(bucket, obj_prefix);
    } catch (std::invalid_argument &e) {
        std::cerr << "gcp_customize_file_system: " << e.what() << std::endl;
        return EINVAL;
    }

    // Map google cloud functions to the file system.
    fs->file_system.fs_directory_list = gcp_object_list;
    fs->file_system.fs_directory_list_single = gcp_object_list_single;
    fs->file_system.fs_directory_list_free = gcp_object_list_free;
    fs->file_system.terminate = gcp_file_system_terminate;
    fs->file_system.fs_exist = gcp_file_exists;
    fs->file_system.fs_open_file = gcp_file_open;
    fs->file_system.fs_remove = gcp_remove;
    fs->file_system.fs_rename = gcp_rename;
    fs->file_system.fs_size = gcp_object_size;

    // Add to the list of the active file systems. Lock will be freed when the scope is exited.
    {
        std::lock_guard<std::mutex> lock_guard(gcp->fs_list_mutex);
        gcp->fs_list.push_back(fs);
    }

    *file_system = &fs->file_system;

    return 0;
}

static int
gcp_add_reference(WT_STORAGE_SOURCE *storage_source)
{
    gcp_store *gcp = reinterpret_cast<gcp_store *>(storage_source);

    if (gcp->reference_count == 0) {
        std::cerr << "gcp_add_reference: gcp storage source extension hasn't been initialized."
                  << std::endl;
        return EINVAL;
    }

    if (gcp->reference_count + 1 == 0) {
        std::cerr << "gcp_add_reference: adding reference will overflow reference count."
                  << std::endl;
        return EINVAL;
    }

    ++gcp->reference_count;

    return 0;
}

// File handle close.
static int
gcp_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    WT_UNUSED(session);

    gcp_file_handle *gcp_fh = reinterpret_cast<gcp_file_handle *>(file_handle);

    // If there are other active instances of the file being open, do not close file handle.
    if (--gcp_fh->reference_count != 0)
        return 0;

    // No more active instances of open file, close the file handle.
    gcp_file_system *fs = gcp_fh->file_system;
    {
        std::lock_guard<std::mutex> lock(fs->fh_list_mutex);
        // Erase remove idiom is used here to remove specific file system.
        fs->fh_list.erase(
          std::remove(fs->fh_list.begin(), fs->fh_list.end(), gcp_fh), fs->fh_list.end());
    }

    delete (gcp_fh);

    return 0;
}

static int
gcp_file_system_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    gcp_file_system *fs = reinterpret_cast<gcp_file_system *>(file_system);
    gcp_store *gcp = reinterpret_cast<gcp_file_system *>(fs)->storage_source;

    WT_UNUSED(session);

    // Remove the current filesystem from the active filesystems list. The lock will be freed when
    // the scope is exited.
    {
        std::lock_guard<std::mutex> lock_guard(gcp->fs_list_mutex);
        // Erase remove idiom is used here to remove specific file system.
        gcp->fs_list.erase(
          std::remove(gcp->fs_list.begin(), gcp->fs_list.end(), fs), gcp->fs_list.end());
    }

    delete (fs);

    return 0;
}

static int
gcp_flush([[maybe_unused]] WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_FILE_SYSTEM *file_system, const char *source, const char *object,
  [[maybe_unused]] const char *config)
{

    gcp_file_system *fs = get_gcp_file_system(file_system);
    WT_FILE_SYSTEM *wt_file_system = fs->wt_file_system;

    // std::filesystem::canonical will throw an exception if object does not exist so
    // check if the object exists.
    if (!std::filesystem::exists(source)) {
        std::cerr << "gcp_flush: Object: " << object << " does not exist." << std::endl;
        return ENOENT;
    }

    // Confirm that the file exists on the native filesystem.
    std::filesystem::path path = std::filesystem::canonical(source);
    bool exist_native = false;
    int ret = wt_file_system->fs_exist(wt_file_system, session, path.c_str(), &exist_native);
    if (ret != 0)
        return ret;
    if (!exist_native)
        return ENOENT;
    return fs->gcp_conn->put_object(object, path);
}

static int
gcp_flush_finish([[maybe_unused]] WT_STORAGE_SOURCE *storage, [[maybe_unused]] WT_SESSION *session,
  WT_FILE_SYSTEM *file_system, const char *source, const char *object,
  [[maybe_unused]] const char *config)
{
    gcp_file_system *fs = get_gcp_file_system(file_system);

    bool exists = false;
    size_t size;
    int ret = fs->gcp_conn->object_exists(object, exists, size);
    if (ret != 0)
        return ret;
    if (!exists)
        return ENOENT;

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
    WT_UNUSED(session);
    gcp_file_system *fs = reinterpret_cast<gcp_file_system *>(file_system);
    *file_handle_ptr = nullptr;

    // Google cloud only supports opening the file in read only mode.
    if ((flags & WT_FS_OPEN_READONLY) == 0 || (flags & WT_FS_OPEN_CREATE) != 0) {
        std::cerr << "gcp_file_open: read-only access required." << std::endl;
        return EINVAL;
    }

    // Only data files and regular files should be opened.
    if (file_type != WT_FS_OPEN_FILE_TYPE_DATA && file_type != WT_FS_OPEN_FILE_TYPE_REGULAR) {
        std::cerr << "gcp_file_open: only data file and regular types supported." << std::endl;
        return EINVAL;
    }

    // Check if object exists in the cloud.
    bool exists;
    size_t size;
    int ret;
    if (ret = (fs->gcp_conn->object_exists(name, exists, size) != 0)) {
        std::cerr << "gcp_file_open: object_exists request to google cloud failed." << std::endl;
        return ret;
    }
    if (!exists) {
        std::cerr << "gcp_file_open: object named " << name << " does not exist in the bucket."
                  << std::endl;
        return EINVAL;
    }

    // Check if there is already an existing file handle open.
    auto fh_iterator = std::find_if(fs->fh_list.begin(), fs->fh_list.end(),
      [name](gcp_file_handle *fh) { return (fh->name.compare(name) == 0); });

    if (fh_iterator != fs->fh_list.end()) {
        (*fh_iterator)->reference_count++;
        *file_handle_ptr = reinterpret_cast<WT_FILE_HANDLE *>(*fh_iterator);

        return 0;
    }

    // If an active file handle does not exist, create a new file handle for the current file.
    gcp_file_handle *gcp_fh;
    try {
        gcp_fh = new gcp_file_handle;
    } catch (std::bad_alloc &e) {
        std::cerr << "gcp_file_open: " << e.what() << std::endl;
        return ENOMEM;
    }
    gcp_fh->file_system = fs;
    gcp_fh->name = name;
    gcp_fh->reference_count = 1;

    // Define functions needed for google cloud with read-only privilleges.
    gcp_fh->fh.close = gcp_file_close;
    gcp_fh->fh.fh_advise = nullptr;
    gcp_fh->fh.fh_extend = nullptr;
    gcp_fh->fh.fh_extend_nolock = nullptr;
    gcp_fh->fh.fh_lock = gcp_file_lock;
    gcp_fh->fh.fh_map = nullptr;
    gcp_fh->fh.fh_map_discard = nullptr;
    gcp_fh->fh.fh_map_preload = nullptr;
    gcp_fh->fh.fh_unmap = nullptr;
    gcp_fh->fh.fh_read = gcp_file_read;
    gcp_fh->fh.fh_size = gcp_file_size;
    gcp_fh->fh.fh_sync = nullptr;
    gcp_fh->fh.fh_sync_nowait = nullptr;
    gcp_fh->fh.fh_truncate = nullptr;
    gcp_fh->fh.fh_write = nullptr;

    // Exclusive access is required when adding file handles to list of file handles. The lock_guard
    // will unlock automatically when the scope is exited.
    {
        std::lock_guard<std::mutex> lock(fs->fh_list_mutex);
        fs->fh_list.push_back(gcp_fh);
    }

    *file_handle_ptr = &gcp_fh->fh;

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
gcp_file_lock([[maybe_unused]] WT_FILE_HANDLE *file_handle, [[maybe_unused]] WT_SESSION *session,
  [[maybe_unused]] bool lock)
{
    // Locks are always granted.
    return 0;
}

static int
gcp_file_size([[maybe_unused]] WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    gcp_file_handle *gcp_fh = reinterpret_cast<gcp_file_handle *>(file_handle);
    gcp_file_system *fs = gcp_fh->file_system;
    bool exists;
    size_t size;
    int ret;
    *sizep = 0;

    // Get file size if the object exists.
    if ((fs->gcp_conn->object_exists(gcp_fh->name, exists, size))) {
        std::cerr << "gcp_file_size: object_exists request to google cloud failed." << std::endl;
        return ret;
    }

    *sizep = size;

    return 0;
}

static int
gcp_object_size(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(sizep);

    return 0;
}

static int
gcp_file_read([[maybe_unused]] WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset,
  size_t len, void *buf)
{
    gcp_file_handle *gcp_fh = reinterpret_cast<gcp_file_handle *>(file_handle);
    gcp_file_system *fs = gcp_fh->file_system;

    int ret;

    if ((ret = fs->gcp_conn->read_object(gcp_fh->name, offset, len, buf)) != 0)
        std::cerr << "gcp_file_read: read attempt failed." << std::endl;

    return ret;
}

static int
gcp_object_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
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

static int
gcp_terminate(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session)
{
    gcp_store *gcp = reinterpret_cast<gcp_store *>(storage_source);

    if (--gcp->reference_count != 0)
        return 0;

    /*
     * Terminate any active filesystems. There are no references to the storage source, so it is
     * safe to walk the active filesystem list without a lock. The removal from the list happens
     * under a lock. Also, removal happens from the front and addition at the end, so we are safe.
     */
    while (!gcp->fs_list.empty()) {
        gcp_file_system *fs = gcp->fs_list.front();
        gcp_file_system_terminate(&fs->file_system, session);
    }

    std::cout << "gcp_terminate: terminated GCP storage source." << std::endl;
    delete (gcp);

    return 0;
}

int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    gcp_store *gcp;
    WT_CONFIG_ITEM v;

    gcp = new gcp_store;
    gcp->wt_api = connection->get_extension_api(connection);
    int ret = gcp->wt_api->config_get(gcp->wt_api, nullptr, config, "verbose.tiered", &v);

    if (ret == 0 && v.val >= WT_VERBOSE_ERROR && v.val <= WT_VERBOSE_DEBUG_5) {
        gcp->verbose = (WT_VERBOSE_LEVEL)v.val;
    } else if (ret != WT_NOTFOUND) {
        std::cerr << "wiredtiger_extension_init: error parsing config for verbosity level." << v.val
                  << std::endl;
        delete (gcp);
        return (ret != 0 ? ret : EINVAL);
    }

    // Allocate a gcp storage structure, with a WT_STORAGE structure as the first field.
    // This allows us to treat references to either type of structure as a reference to the other
    // type.
    gcp->storage_source.ss_customize_file_system = gcp_customize_file_system;
    gcp->storage_source.ss_add_reference = gcp_add_reference;
    gcp->storage_source.terminate = gcp_terminate;
    gcp->storage_source.ss_flush = gcp_flush;
    gcp->storage_source.ss_flush_finish = gcp_flush_finish;

    // The first reference is implied by the call to add_storage_source.
    gcp->reference_count = 1;

    // Load the storage.
    if ((ret = connection->add_storage_source(
           connection, "gcp_store", &gcp->storage_source, nullptr)) != 0) {
        std::cerr << "wiredtiger_extension_init: could not load GCP storage source, shutting down."
                  << std::endl;
        delete (gcp);
    }

    return ret;
}
