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
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include "azure_connection.h"
#include "azure_log_system.h"
#include "wt_internal.h"

#include <azure/core/diagnostics/logger.hpp>

struct azure_file_system;
struct azure_file_handle;

// Statistics to be collected for the Azure blob storage.
struct azure_statistics {
    uint64_t num_list_objects_requests;
    uint64_t num_put_object_requests;
    uint64_t num_read_object_requests;
    uint64_t num_object_exists_requests;
};

struct azure_store {
    WT_STORAGE_SOURCE store;
    WT_EXTENSION_API *wt_api;

    std::mutex fs_mutex;
    std::vector<azure_file_system *> azure_fs;
    uint32_t reference_count;

    int32_t verbose;
    std::unique_ptr<azure_log_system> log;
    azure_statistics statistics;
};

struct azure_file_system {
    WT_FILE_SYSTEM fs;
    azure_store *store;
    WT_FILE_SYSTEM *wt_fs;

    std::mutex fh_mutex;
    std::vector<azure_file_handle *> azure_fh;
    std::unique_ptr<azure_connection> azure_conn;
    std::string home_dir;
};

struct azure_file_handle {
    WT_FILE_HANDLE fh;
    azure_file_system *fs;
    std::string name;
    uint32_t reference_count;
};

// WT_STORAGE_SOURCE Interface
static int azure_customize_file_system(
  WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *, const char *, WT_FILE_SYSTEM **);
static int azure_add_reference(WT_STORAGE_SOURCE *);
static int azure_terminate(WT_STORAGE_SOURCE *, WT_SESSION *);
static int azure_flush(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *, const char *, const char *);
static int azure_flush_finish(
  WT_STORAGE_SOURCE *, WT_SESSION *, WT_FILE_SYSTEM *, const char *, const char *, const char *);

// WT_FILE_SYSTEM Interface
static int azure_object_list(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int azure_object_list_single(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int azure_object_list_free(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static int azure_object_list_add(
  const azure_file_system &, char ***, const std::vector<std::string> &, const uint32_t);
static int azure_file_system_terminate(WT_FILE_SYSTEM *, WT_SESSION *);
static int azure_file_system_exists(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int azure_remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int azure_rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static int azure_object_size(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);
static int azure_file_open(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);

// WT_FILE_HANDLE Interface
static int azure_file_close(WT_FILE_HANDLE *, WT_SESSION *);
static int azure_file_lock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int azure_file_read(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int azure_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);

static void azure_log_statistics(const azure_store &);

// Return a customised file system to access the Azure storage source.
static int
azure_customize_file_system(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  const char *bucket, const char *auth_token, const char *config, WT_FILE_SYSTEM **file_system)
{
    // Get the value of the config key from the string.
    azure_store *azure_storage = reinterpret_cast<azure_store *>(storage_source);
    auto log = azure_storage->log.get();

    int ret;

    if (bucket == nullptr || strlen(bucket) == 0) {
        log->log_err_msg("azure_customize_file_system: Bucket not specified.");
        return EINVAL;
    }

    // Get any prefix to be used for the object keys.
    WT_CONFIG_ITEM obj_prefix_config;
    std::string obj_prefix;

    if ((ret = azure_storage->wt_api->config_get_string(
           azure_storage->wt_api, session, config, "prefix", &obj_prefix_config)) == 0)
        obj_prefix = std::string(obj_prefix_config.str, obj_prefix_config.len);
    else if (ret != WT_NOTFOUND) {
        log->log_err_msg("azure_customize_file_system: error parsing config for object prefix.");
        return ret;
    }

    // Fetch the native WT file system.
    WT_FILE_SYSTEM *wt_file_system;
    if ((ret = azure_storage->wt_api->file_system_get(
           azure_storage->wt_api, session, &wt_file_system)) != 0)
        return ret;

    // Create file system and allocate memory for the file system.
    azure_file_system *azure_fs;
    try {
        azure_fs = new azure_file_system;
    } catch (std::bad_alloc &e) {
        log->log_err_msg(std::string("azure_customize_file_system: ") + e.what());
        return ENOMEM;
    }

    // Initialise references to azure storage source, wt fs and home directory.
    azure_fs->store = azure_storage;
    azure_fs->wt_fs = wt_file_system;
    azure_fs->home_dir = session->connection->get_home(session->connection);
    try {
        azure_fs->azure_conn = std::make_unique<azure_connection>(bucket, obj_prefix);
    } catch (std::runtime_error &e) {
        log->log_err_msg(std::string("azure_customize_file_system: ") + e.what());
        return ENOENT;
    }
    azure_fs->fs.fs_directory_list = azure_object_list;
    azure_fs->fs.fs_directory_list_single = azure_object_list_single;
    azure_fs->fs.fs_directory_list_free = azure_object_list_free;
    azure_fs->fs.terminate = azure_file_system_terminate;
    azure_fs->fs.fs_exist = azure_file_system_exists;
    azure_fs->fs.fs_open_file = azure_file_open;
    azure_fs->fs.fs_remove = azure_remove;
    azure_fs->fs.fs_rename = azure_rename;
    azure_fs->fs.fs_size = azure_object_size;

    // Add to the list of the active file systems. Lock will be freed when the scope is exited.
    {
        std::lock_guard<std::mutex> lock_guard(azure_storage->fs_mutex);
        azure_storage->azure_fs.push_back(azure_fs);
    }
    *file_system = &azure_fs->fs;
    return ret;
}

// Add a reference to the storage source so we can reference count to know when to terminate.
static int
azure_add_reference(WT_STORAGE_SOURCE *storage_source)
{
    azure_store *azure_storage = reinterpret_cast<azure_store *>(storage_source);
    auto log = azure_storage->log.get();

    if (azure_storage->reference_count == 0 || azure_storage->reference_count + 1 == 0) {
        log->log_err_msg("azure_add_reference: missing reference or overflow.");
        return EINVAL;
    }
    ++azure_storage->reference_count;
    return 0;
}

// Flush given file to the Azure Blob storage.
static int
azure_flush(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session, WT_FILE_SYSTEM *file_system,
  const char *source, const char *object, const char *config)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    WT_FILE_SYSTEM *wtFileSystem = azure_fs->wt_fs;
    auto log = azure_fs->store->log.get();
    std::string local_file_path = azure_fs->home_dir + "/" + source;

    WT_UNUSED(storage_source);
    WT_UNUSED(source);
    WT_UNUSED(config);

    azure_fs->store->statistics.num_put_object_requests++;
    // std::filesystem::canonical will throw an exception if object does not exist so
    // check if the object exists.
    if (!std::filesystem::exists(local_file_path)) {
        log->log_err_msg("azure_flush: No such file " + local_file_path + ".");
        return ENOENT;
    }

    bool exists_native = false;
    int ret = wtFileSystem->fs_exist(wtFileSystem, session,
      std::filesystem::canonical(local_file_path).string().c_str(), &exists_native);
    if (ret != 0) {
        log->log_err_msg("azure_flush: Failed to check for the existence of " +
          std::string(source) + " on the native filesystem.");
        return ret;
    }

    if (!exists_native) {
        log->log_err_msg("azure_flush: No such file" + std::string(object) + ".");
        return ENOENT;
    }
    log->log_debug_message(
      "azure_flush: Uploading object: " + std::string(object) + " into bucket using put_object.");

    // Upload the object into the bucket.
    if (azure_fs->azure_conn->put_object(object, std::filesystem::canonical(local_file_path)) != 0)
        log->log_err_msg("azure_flush: Put object request to Azure failed.");
    else
        log->log_debug_message("azure_flush: Uploaded object to Azure.");
    return ret;
}

// Check that flush has been completed by checking the object exists in Azure.
static int
azure_flush_finish(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session,
  WT_FILE_SYSTEM *file_system, const char *source, const char *object, const char *config)
{
    azure_store *azure_storage = reinterpret_cast<azure_store *>(storage_source);
    size_t size = 0;
    auto log = azure_storage->log.get();

    WT_UNUSED(config);
    WT_UNUSED(source);
    WT_UNUSED(size);

    bool existp = false;
    log->log_debug_message(
      "azure_flush_finish: Checking object: " + std::string(object) + " exists in Azure.");
    return azure_file_system_exists(file_system, session, object, &existp);
}

// Discard any resources on termination.
static int
azure_terminate(WT_STORAGE_SOURCE *storage_source, WT_SESSION *session)
{
    azure_store *azure_storage = reinterpret_cast<azure_store *>(storage_source);

    if (--azure_storage->reference_count != 0)
        return 0;

    /*
     * Terminate any active filesystems. There are no references to the storage source, so it is
     * safe to walk the active filesystem list without a lock. The removal from the list happens
     * under a lock. Also, removal happens from the front and addition at the end, so we are safe.
     */
    while (!azure_storage->azure_fs.empty()) {
        WT_FILE_SYSTEM *fs = reinterpret_cast<WT_FILE_SYSTEM *>(azure_storage->azure_fs.front());
        azure_file_system_terminate(fs, session);
    }

    azure_log_statistics(*azure_storage);
    delete azure_storage;
    return 0;
}

// Helper to return a list of object names for the given location.
static int
azure_object_list_helper(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *search_prefix, char ***dirlistp, uint32_t *countp, bool list_single)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();
    std::vector<std::string> objects;
    std::string complete_prefix;

    *countp = 0;
    azure_fs->store->statistics.num_list_objects_requests++;

    if (directory != nullptr) {
        complete_prefix.append(directory);
        // Add a terminating '/' if one doesn't exist.
        if (complete_prefix.length() > 1 && complete_prefix.back() != '/')
            complete_prefix.push_back('/');
    }
    if (search_prefix != nullptr)
        complete_prefix.append(search_prefix);

    int ret;

    ret = list_single ? azure_fs->azure_conn->list_objects(complete_prefix, objects, true) :
                        azure_fs->azure_conn->list_objects(complete_prefix, objects, false);

    if (ret != 0) {
        log->log_err_msg("azure_object_list: list_objects request to Azure failed.");
        return ret;
    }
    *countp = objects.size();

    log->log_debug_message("azure_object_list: list_objects request to Azure succeeded. Received " +
      std::to_string(objects.size()) + " objects.");
    azure_object_list_add(*azure_fs, dirlistp, objects, *countp);

    return ret;
}

// Return a list of object names for the given location.
static int
azure_object_list(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return azure_object_list_helper(
      file_system, session, directory, prefix, dirlistp, countp, false);
}

// Return a single object name for the given location.
static int
azure_object_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return azure_object_list_helper(
      file_system, session, directory, prefix, dirlistp, countp, true);
}

// Free memory allocated by azure_object_list.
static int
azure_object_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, char **dirlist, uint32_t count)
{
    WT_UNUSED(file_system);
    WT_UNUSED(session);

    if (dirlist != nullptr) {
        while (count > 0)
            free(dirlist[--count]);
        free(dirlist);
    }

    return 0;
}

// Add objects retrieved from Azure bucket into the object list, and allocate the memory needed.
static int
azure_object_list_add(const azure_file_system &azure_fs, char ***dirlistp,
  const std::vector<std::string> &objects, const uint32_t count)
{
    auto log = azure_fs.store->log.get();
    char **entries;
    if ((entries = reinterpret_cast<char **>(malloc(sizeof(char *) * count))) == nullptr) {
        log->log_err_msg("azure_object_list_add: Unable to allocate memory for object list.");
        return ENOMEM;
    }

    // Populate entries with the object string.
    for (int i = 0; i < count; i++) {
        if ((entries[i] = strdup(objects[i].c_str())) == nullptr) {
            log->log_err_msg("azure_object_list_add: Unable to allocate memory for object string.");
            return ENOMEM;
        }
    }

    *dirlistp = entries;

    return 0;
}

// Discard any resources on termination of the file system.
static int
azure_file_system_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *session)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    azure_store *azure_storage = azure_fs->store;

    WT_UNUSED(session);

    // Remove from the active file system list. The lock will be freed when the scope is exited.
    {
        std::lock_guard<std::mutex> lock_guard(azure_storage->fs_mutex);
        // Erase-remove idiom used to eliminate specific file system.
        azure_storage->azure_fs.erase(
          std::remove(azure_storage->azure_fs.begin(), azure_storage->azure_fs.end(), azure_fs),
          azure_storage->azure_fs.end());
    }
    azure_fs->azure_conn.reset();
    free(azure_fs);

    return 0;
}

// Check if the object exists in the Azure storage source.
static int
azure_file_system_exists(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, bool *existp)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();
    size_t size = 0;

    WT_UNUSED(session);
    WT_UNUSED(size);
    azure_fs->store->statistics.num_object_exists_requests++;

    log->log_debug_message(
      "azure_file_system_exists: Checking object: " + std::string(name) + " exists in Azure.");
    // Check whether the object exists in the cloud.
    int ret;
    if (ret = azure_fs->azure_conn->object_exists(name, *existp, size) != 0) {
        log->log_err_msg(
          "azure_file_system_exists: Error with searching for object: " + std::string(name));
        return ret;
    }

    if (!*existp)
        log->log_debug_message(
          "azure_file_system_exists: Object: " + std::string(name) + " does not exist in Azure.");
    else
        log->log_debug_message(
          "azure_file_system_exists: Object: " + std::string(name) + " exists in Azure.");
    return 0;
}

// POSIX remove, not supported for cloud objects.
static int
azure_remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();

    WT_UNUSED(session);
    WT_UNUSED(name);
    WT_UNUSED(flags);

    log->log_err_msg(
      "azure_remove: Object: " + std::string(name) + ": remove of file not supported.");
    return ENOTSUP;
}

// POSIX rename, not supported for cloud objects.
static int
azure_rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();

    WT_UNUSED(session);
    WT_UNUSED(from);
    WT_UNUSED(to);
    WT_UNUSED(flags);

    log->log_err_msg(
      "azure_rename: Object: " + std::string(from) + ": rename of file not supported.");
    return ENOTSUP;
}

// Get the size of a file in bytes, by file name.
static int
azure_object_size(
  WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();

    WT_UNUSED(session);

    int ret;
    bool exists;
    size_t size = 0;
    *sizep = 0;

    if ((ret = azure_fs->azure_conn->object_exists(name, exists, size)) != 0) {
        log->log_err_msg("azure_object_size: object_exists request to Azure failed.");
        return ret;
    }

    *sizep = size;

    return 0;
}

// File open for the Azure storage source.
static int
azure_file_open(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE file_type, uint32_t flags, WT_FILE_HANDLE **file_handlep)
{
    azure_file_system *azure_fs = reinterpret_cast<azure_file_system *>(file_system);
    auto log = azure_fs->store->log.get();

    WT_UNUSED(session);

    // Azure only supports opening the file in read only mode.
    if ((flags & WT_FS_OPEN_READONLY) == 0 || (flags & WT_FS_OPEN_CREATE) != 0) {
        log->log_err_msg("azure_file_open: read-only access required.");
        return EINVAL;
    }

    // Only data files and regular files should be opened.
    if (file_type != WT_FS_OPEN_FILE_TYPE_DATA && file_type != WT_FS_OPEN_FILE_TYPE_REGULAR) {
        log->log_err_msg("azure_file_open: only data file and regular types supported.");
        return EINVAL;
    }

    // Check if object exists.
    bool exists;
    int ret;
    size_t size = 0;

    WT_UNUSED(size);

    if ((ret = azure_fs->azure_conn->object_exists(std::string(name), exists, size)) != 0) {
        log->log_err_msg("azure_file_open: object_exists request to Azure failed.");
        return ret;
    }
    if (!exists) {
        log->log_err_msg("azure_file_open: no such file named " + std::string(name) + ".");
        return EINVAL;
    }

    // Check if there is already an existing file handle open.
    auto fh_iterator = std::find_if(azure_fs->azure_fh.begin(), azure_fs->azure_fh.end(),
      [name](azure_file_handle *fh) { return fh->name.compare(name) == 0; });

    // Active file handle for file exists, increment reference count.
    if (fh_iterator != azure_fs->azure_fh.end()) {
        (*fh_iterator)->reference_count++;
        *file_handlep = reinterpret_cast<WT_FILE_HANDLE *>(*fh_iterator);
        return 0;
    }

    // No active file handle, create a new file handle.
    azure_file_handle *azure_fh;
    try {
        azure_fh = new azure_file_handle;
    } catch (std::bad_alloc &e) {
        log->log_err_msg(std::string("azure_file_open: ") + e.what());
        return ENOMEM;
    }
    azure_fh->name = name;
    azure_fh->reference_count = 1;
    azure_fh->fs = azure_fs;

    // Define functions needed for Azure with read-only privilleges.
    azure_fh->fh.close = azure_file_close;
    azure_fh->fh.fh_advise = nullptr;
    azure_fh->fh.fh_extend = nullptr;
    azure_fh->fh.fh_extend_nolock = nullptr;
    azure_fh->fh.fh_lock = azure_file_lock;
    azure_fh->fh.fh_map = nullptr;
    azure_fh->fh.fh_map_discard = nullptr;
    azure_fh->fh.fh_unmap = nullptr;
    azure_fh->fh.fh_read = azure_file_read;
    azure_fh->fh.fh_size = azure_file_size;
    azure_fh->fh.fh_sync = nullptr;
    azure_fh->fh.fh_sync_nowait = nullptr;
    azure_fh->fh.fh_truncate = nullptr;
    azure_fh->fh.fh_write = nullptr;
    azure_fh->fh.name = strdup(name);

    // Exclusive Access is required when adding file handles to list of file handles.
    // lock_guard will unlock automatically when the scope is exited.
    {
        std::lock_guard<std::mutex> lock_guard(azure_fs->fh_mutex);
        azure_fs->azure_fh.push_back(azure_fh);
    }
    *file_handlep = &azure_fh->fh;

    return 0;
}

// File handle close.
static int
azure_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *session)
{
    azure_file_handle *azure_fh = reinterpret_cast<azure_file_handle *>(file_handle);

    WT_UNUSED(session);

    // If there are other active instances of the file being open, do not close file handle.
    if (--azure_fh->reference_count != 0)
        return 0;

    // No more active instances of open file, close the file handle.
    azure_file_system *azure_fs = azure_fh->fs;
    {
        std::lock_guard<std::mutex> lock_guard(azure_fs->fh_mutex);
        // Erase-remove idiom to eliminate specific file handle
        azure_fs->azure_fh.erase(
          std::remove(azure_fs->azure_fh.begin(), azure_fs->azure_fh.end(), azure_fh),
          azure_fs->azure_fh.end());
    }

    return 0;
}

// Lock/unlock a file.
static int
azure_file_lock(WT_FILE_HANDLE *file_handle, WT_SESSION *session, bool lock)
{
    // Since the file is in the cloud, locks are always granted because concurrent reads do not
    // require a lock.
    WT_UNUSED(file_handle);
    WT_UNUSED(session);
    WT_UNUSED(lock);

    return 0;
}

// Read a file using Azure connection class read object functionality.
static int
azure_file_read(
  WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    azure_file_handle *azure_fh = reinterpret_cast<azure_file_handle *>(file_handle);
    azure_file_system *azure_fs = azure_fh->fs;
    auto log = azure_fs->store->log.get();

    WT_UNUSED(session);

    azure_fs->store->statistics.num_read_object_requests++;
    int ret;
    if ((ret = azure_fs->azure_conn->read_object(azure_fh->name, offset, len, buf) != 0)) {
        log->log_err_msg("azure_file_read: read_object request to Azure failed.");
        return ret;
    }

    return 0;
}

// Get the size of a file in bytes.
static int
azure_file_size(WT_FILE_HANDLE *file_handle, WT_SESSION *session, wt_off_t *sizep)
{
    azure_file_handle *azure_fh = reinterpret_cast<azure_file_handle *>(file_handle);
    azure_file_system *azure_fs = azure_fh->fs;
    auto log = azure_fs->store->log.get();

    WT_DECL_RET;
    bool exists;
    size_t size = 0;
    *sizep = 0;

    if ((ret = azure_fh->fs->azure_conn->object_exists(azure_fh->name, exists, size)) != 0) {
        log->log_err_msg("azure_file_size: object_exists request to Azure failed.");
        return ret;
    }

    *sizep = size;

    return 0;
}

// Log collected statistics.
static void
azure_log_statistics(const azure_store &azure_storage)
{
    azure_storage.log->log_debug_message("Azure list objects count: " +
      std::to_string(azure_storage.statistics.num_list_objects_requests));
    azure_storage.log->log_debug_message("Azure put object count: " +
      std::to_string(azure_storage.statistics.num_put_object_requests));
    azure_storage.log->log_debug_message("Azure get object count: " +
      std::to_string(azure_storage.statistics.num_read_object_requests));
    azure_storage.log->log_debug_message("Azure object exists count: " +
      std::to_string(azure_storage.statistics.num_object_exists_requests));
}

// An Azure storage source library - creates an entry point to the Azure extension.
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    azure_store *azure_storage = new azure_store;
    WT_CONFIG_ITEM v;

    azure_storage->wt_api = connection->get_extension_api(connection);
    int ret = azure_storage->wt_api->config_get(
      azure_storage->wt_api, nullptr, config, "verbose.tiered", &v);

    azure_storage->verbose = WT_VERBOSE_ERROR;
    // Initialise logger for the storage source.
    if (ret == 0 && v.val >= WT_VERBOSE_ERROR && v.val <= WT_VERBOSE_DEBUG_5) {
        azure_storage->verbose = v.val;
        azure_storage->log->set_wt_verbosity_level(azure_storage->verbose);
    } else if (ret != WT_NOTFOUND) {
        azure_storage->log->log_err_msg(
          "wiredtiger_extension_init: error parsing config for verbose level.");
        delete azure_storage;
        return ret != 0 ? ret : EINVAL;
    }

    azure_storage->statistics = {};
    azure_storage->log =
      std::make_unique<azure_log_system>(azure_storage->wt_api, azure_storage->verbose);
    Azure::Core::Diagnostics::Logger::SetListener(
      [azure_storage](auto lvl, auto msg) { azure_storage->log->azure_log_listener(lvl, msg); });

    azure_storage->store.ss_customize_file_system = azure_customize_file_system;
    azure_storage->store.ss_add_reference = azure_add_reference;
    azure_storage->store.terminate = azure_terminate;
    azure_storage->store.ss_flush = azure_flush;
    azure_storage->store.ss_flush_finish = azure_flush_finish;

    // The first reference is implied by the call to add_storage_source.
    azure_storage->reference_count = 1;

    // Load the storage.
    if ((connection->add_storage_source(
          connection, "azure_store", &azure_storage->store, nullptr)) != 0) {
        azure_storage->log->log_err_msg(
          "wiredtiger_extension_init: Could not load Azure storage source, shutting down.");
        delete azure_storage;
        return -1;
    }
    return 0;
}
