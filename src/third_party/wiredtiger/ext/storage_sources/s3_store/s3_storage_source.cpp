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
#include <fstream>
#include <list>
#include <errno.h>
#include <filesystem>
#include <mutex>

#include "s3_connection.h"
#include "s3_log_system.h"
#include "s3_aws_manager.h"

#include <aws/auth/credentials.h>
#include <aws/core/Aws.h>
#include <aws/core/utils/logging/AWSLogging.h>

#define UNUSED(x) (void)(x)
#define FS2S3(fs) (((S3FileSystem *)(fs))->storage)

struct S3FileHandle;
struct S3FileSystem;

// Statistics to be collected for the S3 storage.
struct S3Statistics {
    // Operations using AWS SDK.
    uint64_t listObjectsCount;  // Number of S3 list objects requests
    uint64_t putObjectCount;    // Number of S3 put object requests
    uint64_t getObjectCount;    // Number of S3 get object requests
    uint64_t objectExistsCount; // Number of S3 object exists requests

    // Operations using WiredTiger's native file handle operations.
    uint64_t fhOps;     // Number of non read/write file handle operations
    uint64_t fhReadOps; // Number of file handle read operations
};

// S3 storage source structure.
struct S3Storage {
    WT_STORAGE_SOURCE storageSource; // Must come first
    WT_EXTENSION_API *wtApi;         // Extension API
    std::shared_ptr<S3LogSystem> log;

    std::mutex fsListMutex;           // Protect the file system list
    std::list<S3FileSystem *> fsList; // List of initiated file systems
    std::mutex fhMutex;               // Protect the file handle list
    std::list<S3FileHandle *> fhList; // List of open file handles

    uint32_t referenceCount; // Number of references to this storage source
    int32_t verbose;

    S3Statistics statistics;
};

struct S3FileSystem {
    // Must come first - this is the interface for the file system we are implementing.
    WT_FILE_SYSTEM fileSystem;
    S3Storage *storage;
    // The S3_FILE_SYSTEM is built on top of the WT_FILE_SYSTEM. We require an instance of the
    // WT_FILE_SYSTEM in order to access the native WiredTiger filesystem functionality, such as the
    // native WT file handle open.
    WT_FILE_SYSTEM *wtFileSystem;
    S3Connection *connection;
    std::string cacheDir; // Directory for cached objects
    std::string homeDir;  // Owned by the connection
};

struct S3FileHandle {
    WT_FILE_HANDLE iface; // Must come first
    S3Storage *storage;   // Enclosing storage source

    // Similarly, The S3FileHandle is built on top of the WT_FILE_HANDLE. We require an instance of
    // the WT_FILE_HANDLE in order to access the native WiredTiger filehandle functionality, such as
    // the native WT file handle read and close.

    WT_FILE_HANDLE *wtFileHandle;
};

// Configuration variables for connecting to S3CrtClient.
const double throughputTargetGbps = 5;
const uint64_t partSize = 8 * 1024 * 1024; // 8 MB.

// Define the AwsManager class
std::mutex AwsManager::InitGuard;
AwsManager AwsManager::aws_instance;

static int S3GetDirectory(
  const S3Storage &, const std::string &, const std::string &, bool, std::string &);
static bool S3CacheExists(WT_FILE_SYSTEM *, const std::string &);
static std::string S3Path(const std::string &, const std::string &);
static int S3FileExists(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int S3CustomizeFileSystem(
  WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *, const char *, WT_FILE_SYSTEM **);
static int S3AddReference(WT_STORAGE_SOURCE *);
static int S3FileSystemTerminate(WT_FILE_SYSTEM *, WT_SESSION *);
static int S3FileOpen(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static int S3Remove(WT_FILE_SYSTEM *, WT_SESSION *, const char *, uint32_t);
static int S3Rename(WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, uint32_t);
static bool LocalFileExists(const std::string &);
static int S3FileRead(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int S3ObjectList(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListAdd(
  const S3Storage &, char ***, const std::vector<std::string> &, const uint32_t);
static int S3ObjectListSingle(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListFree(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static void S3LogStatistics(const S3Storage &);

static int S3FileClose(WT_FILE_HANDLE *, WT_SESSION *);
static int S3FileSize(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);
static int S3FileLock(WT_FILE_HANDLE *, WT_SESSION *, bool);
static int S3ObjectSize(WT_FILE_SYSTEM *, WT_SESSION *, const char *, wt_off_t *);

// Construct a pathname from the directory and the object name.
static std::string
S3Path(const std::string &dir, const std::string &name)
{
    // Skip over "./" and variations (".//", ".///./././//") at the beginning of the name.
    int i = 0;
    while (name[i] == '.') {
        if (name[1] != '/')
            break;
        i += 2;
        while (name[i] == '/')
            i++;
    }
    std::string strippedName = name.substr(i, name.length() - i);
    return (dir + "/" + strippedName);
}

// Return if the file exists. First checks the cache, and then the S3 Bucket.
static int
S3FileExists(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *name, bool *fileExists)
{
    int ret = 0;
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    S3Storage *s3 = FS2S3(fileSystem);

    // Check if file exists in the cache.
    *fileExists = S3CacheExists(fileSystem, name);
    if (*fileExists) {
        s3->log->LogDebugMessage("S3FileExists: Found file in cache.");
        return (ret);
    }

    // It's not in the cache, try the S3 bucket.
    size_t objectSize;
    s3->statistics.objectExistsCount++;
    if ((ret = fs->connection->ObjectExists(name, *fileExists, objectSize)) != 0)
        s3->log->LogErrorMessage("S3FileExists: ObjectExists request to S3 failed.");
    else
        s3->log->LogDebugMessage("S3FileExists: Found file in S3.");

    return (ret);
}

// Checks whether the given file exists in the cache.
static bool
S3CacheExists(WT_FILE_SYSTEM *fileSystem, const std::string &name)
{
    const std::string path = S3Path(((S3FileSystem *)fileSystem)->cacheDir, name);
    return (LocalFileExists(path));
}

// Checks whether a file corresponding to the provided path exists locally.
static bool
LocalFileExists(const std::string &path)
{
    std::ifstream f(path);
    return (f.good());
}

// Return a copy of a directory name after verifying that it is a directory.
static int
S3GetDirectory(const S3Storage &s3, const std::string &home, const std::string &name, bool create,
  std::string &copy)
{
    // copy must be initialised before the function returns.
    copy = "";

    int ret;
    std::string dirName;

    // For relative pathnames, the path is considered to be relative to the home directory.
    if (name[0] == '/')
        dirName = name;
    else
        dirName = home + "/" + name;

    // Use filesystem status to find if directory exists.
    std::error_code ec;
    std::filesystem::file_status status = std::filesystem::status(dirName.c_str(), ec);

    if (!std::filesystem::exists(status) && create) {
        try {
            std::filesystem::create_directory(dirName.c_str());
            std::filesystem::permissions(dirName.c_str(), std::filesystem::perms::all);
        } catch (std::filesystem::filesystem_error const &e) {
            s3.log->LogErrorMessage(std::string("S3GetDirectory: ") + e.what());
        }

        s3.log->LogDebugMessage("S3GetDirectory: Successfully created directory.");
    }

    status = std::filesystem::status(dirName.c_str(), ec);
    ret = ec.value();

    if (ret != 0) {
        s3.log->LogErrorMessage("S3GetDirectory: No such file or directory");
    } else if (!std::filesystem::is_directory(status)) {
        s3.log->LogErrorMessage("S3GetDirectory: invalid directory name");
        ret = EINVAL;
    }

    copy = dirName;
    return (ret);
}

// File handle close.
static int
S3FileClose(WT_FILE_HANDLE *fileHandle, WT_SESSION *session)
{
    int ret = 0;
    S3FileHandle *s3FileHandle = (S3FileHandle *)fileHandle;
    S3Storage *s3 = s3FileHandle->storage;
    WT_FILE_HANDLE *wtFileHandle = s3FileHandle->wtFileHandle;

    // We require exclusive access to the list of file handles when removing file handles. The
    // lock_guard will be unlocked automatically once the scope is exited.
    {
        std::lock_guard<std::mutex> lock(s3->fhMutex);
        s3->fhList.remove(s3FileHandle);
    }
    if (wtFileHandle != nullptr) {
        s3->statistics.fhOps++;
        if ((ret = wtFileHandle->close(wtFileHandle, session)) != 0)
            s3->log->LogErrorMessage("S3FileClose: close file handle failed.");
        else
            s3->log->LogDebugMessage("S3FileClose: Successfully closed file handle.");
    }

    free(s3FileHandle->iface.name);
    free(s3FileHandle);
    return (ret);
}

// File open for the s3 storage source.
static int
S3FileOpen(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE fileType, uint32_t flags, WT_FILE_HANDLE **fileHandlePtr)
{
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    S3Storage *s3 = FS2S3(fileSystem);
    int ret;

    *fileHandlePtr = nullptr;

    // We only support opening the file in read only mode.
    if ((flags & WT_FS_OPEN_READONLY) == 0 || (flags & WT_FS_OPEN_CREATE) != 0) {
        s3->log->LogErrorMessage("S3FileOpen: read-only access required.");
        return (EINVAL);
    }

    // Currently, only data files should be being opened; although this constraint can be relaxed in
    // the future.
    if (fileType != WT_FS_OPEN_FILE_TYPE_DATA && fileType != WT_FS_OPEN_FILE_TYPE_REGULAR) {
        s3->log->LogErrorMessage("S3FileOpen: only data file and regular types supported.");
        return (EINVAL);
    }

    S3FileHandle *s3FileHandle;
    if ((s3FileHandle = (S3FileHandle *)calloc(1, sizeof(S3FileHandle))) == nullptr) {
        s3->log->LogErrorMessage("S3FileOpen: unable to allocate memory for file handle.");
        return (ENOMEM);
    }

    // Make a copy from S3 if the file is not in the cache.
    const std::string cachePath = S3Path(fs->cacheDir, name);
    if (!LocalFileExists(cachePath)) {
        s3->statistics.getObjectCount++;
        if ((ret = fs->connection->GetObject(name, cachePath)) != 0) {
            s3->log->LogErrorMessage("S3FileOpen: GetObject request to S3 failed.");
            return (ret);
        }
    }

    // Use WiredTiger's native file handle open.
    WT_FILE_SYSTEM *wtFileSystem = fs->wtFileSystem;
    WT_FILE_HANDLE *wtFileHandle;
    ret = wtFileSystem->fs_open_file(
      wtFileSystem, session, cachePath.c_str(), fileType, flags, &wtFileHandle);
    if (ret != 0) {
        s3->log->LogErrorMessage("S3FileOpen: fs_open_file failed.");
        return (ret);
    } else
        s3->log->LogDebugMessage("S3FileOpen: fs_open_file succeeded.");

    s3FileHandle->wtFileHandle = wtFileHandle;
    s3FileHandle->storage = s3;

    // We only define the functions we need since S3 is read-only.
    WT_FILE_HANDLE *fileHandle = (WT_FILE_HANDLE *)s3FileHandle;
    fileHandle->close = S3FileClose;
    fileHandle->fh_advise = nullptr;
    fileHandle->fh_extend = nullptr;
    fileHandle->fh_extend_nolock = nullptr;
    fileHandle->fh_lock = S3FileLock;
    fileHandle->fh_map = nullptr;
    fileHandle->fh_map_discard = nullptr;
    fileHandle->fh_map_preload = nullptr;
    fileHandle->fh_unmap = nullptr;
    fileHandle->fh_read = S3FileRead;
    fileHandle->fh_size = S3FileSize;
    fileHandle->fh_sync = nullptr;
    fileHandle->fh_sync_nowait = nullptr;
    fileHandle->fh_truncate = nullptr;
    fileHandle->fh_write = nullptr;

    fileHandle->name = strdup(name);
    if (fileHandle->name == nullptr) {
        s3->log->LogErrorMessage("S3FileOpen: unable to allocate memory for object name.");
        return (ENOMEM);
    }

    // We require exclusive access to the list of file handles when adding file handles to it. The
    // lock_guard will be unlocked automatically when the scope is exited.
    {
        std::lock_guard<std::mutex> lock(s3->fhMutex);
        s3FileHandle->storage->fhList.push_back(s3FileHandle);
    }

    *fileHandlePtr = fileHandle;
    return (0);
}

// POSIX rename, not supported for cloud objects.
static int
S3Rename(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *from, const char *to,
  uint32_t flags)
{
    S3Storage *s3 = FS2S3(file_system);

    UNUSED(to);
    UNUSED(flags);

    s3->log->LogErrorMessage(std::string(from) + ": rename of file not supported");
    return (ENOTSUP);
}

// POSIX remove, not supported for cloud objects.
static int
S3Remove(WT_FILE_SYSTEM *file_system, WT_SESSION *session, const char *name, uint32_t flags)
{
    S3Storage *s3 = FS2S3(file_system);

    UNUSED(flags);

    s3->log->LogErrorMessage(std::string(name) + ": remove of file not supported");
    return (ENOTSUP);
}

// Get the size of a file in bytes, by file name.

static int
S3ObjectSize(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *name, wt_off_t *sizep)
{
    // sizep must be initialised before the function returns.
    *sizep = 0;

    S3Storage *s3 = FS2S3(fileSystem);
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    size_t objectSize;
    bool exists;
    int ret;

    s3->statistics.objectExistsCount++;
    if ((ret = fs->connection->ObjectExists(name, exists, objectSize)) != 0)
        return (ret);

    s3->log->LogDebugMessage(
      "S3ObjectSize: Found S3 object size to be " + std::to_string(objectSize) + " bytes.");
    *sizep = objectSize;
    return (ret);
}

// Lock/unlock a file.
static int
S3FileLock(WT_FILE_HANDLE *fileHandle, WT_SESSION *session, bool lock)
{
    // Locks are always granted.
    UNUSED(session);
    UNUSED(lock);

    return (0);
}

// Read a file using WiredTiger's native file handle read.

static int
S3FileRead(WT_FILE_HANDLE *fileHandle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    S3FileHandle *s3FileHandle = (S3FileHandle *)fileHandle;
    S3Storage *s3 = s3FileHandle->storage;
    WT_FILE_HANDLE *wtFileHandle = s3FileHandle->wtFileHandle;
    int ret;
    s3->statistics.fhReadOps++;
    if ((ret = wtFileHandle->fh_read(wtFileHandle, session, offset, len, buf)) != 0)
        s3->log->LogErrorMessage("S3FileRead: fh_read failed.");
    else
        s3->log->LogDebugMessage(
          "S3FileRead: fh_read succeeded in reading " + std::to_string(len) + " bytes.");
    return (ret);
}

// Get the size of a file in bytes, by file handle.
static int
S3FileSize(WT_FILE_HANDLE *fileHandle, WT_SESSION *session, wt_off_t *sizep)
{
    S3FileHandle *s3FileHandle = (S3FileHandle *)fileHandle;
    S3Storage *s3 = s3FileHandle->storage;
    WT_FILE_HANDLE *wtFileHandle = s3FileHandle->wtFileHandle;
    s3->statistics.fhOps++;
    return (wtFileHandle->fh_size(wtFileHandle, session, sizep));
}

// Return a customized file system to access the s3 storage source objects. The authToken
// contains the AWS access key ID and the AWS secret key as comma-separated values.
static int
S3CustomizeFileSystem(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session, const char *bucket,
  const char *authToken, const char *config, WT_FILE_SYSTEM **fileSystem)
{
    S3Storage *s3;
    int ret;

    s3 = (S3Storage *)storageSource;

    // We need to have a bucket to setup the file system. The bucket is expected to be a name and a
    // region, separated by a semi-colon. eg: 'abcd;ap-southeast-2'.

    if (bucket == nullptr || strlen(bucket) == 0) {
        s3->log->LogErrorMessage("S3CustomizeFileSystem: bucket not specified.");
        return (EINVAL);
    }
    int delimiter = std::string(bucket).find(';');
    if (delimiter == std::string::npos || delimiter == 0 || delimiter == strlen(bucket) - 1) {
        s3->log->LogErrorMessage(
          "S3CustomizeFileSystem: improper bucket name, "
          "should be a name and a region separated by a semicolon.");
        return (EINVAL);
    }
    const std::string bucketName = std::string(bucket).substr(0, delimiter);
    const std::string region = std::string(bucket).substr(delimiter + 1);

    // Fail if there is no authentication provided.
    if (authToken == nullptr || strlen(authToken) == 0) {
        s3->log->LogErrorMessage("S3CustomizeFileSystem: authToken not specified.");
        return (EINVAL);
    }

    // An auth token is needed to setup the file system. The token is expected to be an access key
    // and a secret key separated by a semi-colon.
    if (authToken == nullptr || strlen(authToken) == 0) {
        s3->log->LogErrorMessage("S3CustomizeFileSystem: auth token not specified.");
        return (EINVAL);
    }
    delimiter = std::string(authToken).find(';');
    if (delimiter == std::string::npos || delimiter == 0 || delimiter == strlen(authToken) - 1) {
        s3->log->LogErrorMessage(
          "S3CustomizeFileSystem: improper authToken, should be an access key and a secret key "
          "separated by a semicolon.");
        return (EINVAL);
    }
    const std::string accessKeyId = std::string(authToken).substr(0, delimiter);
    const std::string secretKey = std::string(authToken).substr(delimiter + 1);

    Aws::Auth::AWSCredentials credentials;
    credentials.SetAWSAccessKeyId(accessKeyId);
    credentials.SetAWSSecretKey(secretKey);

    s3->log->LogDebugMessage(
      "S3CustomizeFileSystem: AWS access key and secret key set successfully.");

    // Parse configuration string.

    // Get any prefix to be used for the object keys.
    WT_CONFIG_ITEM objPrefixConf;
    std::string objPrefix;
    if ((ret = s3->wtApi->config_get_string(
           s3->wtApi, session, config, "prefix", &objPrefixConf)) == 0)
        objPrefix = std::string(objPrefixConf.str, objPrefixConf.len);
    else if (ret != WT_NOTFOUND) {
        s3->log->LogErrorMessage("S3CustomizeFileSystem: error parsing config for object prefix.");
        return (ret);
    }

    // Configure the AWS Client configuration.
    Aws::S3Crt::ClientConfiguration awsConfig;
    awsConfig.partSize = partSize;
    awsConfig.region = region;
    awsConfig.throughputTargetGbps = throughputTargetGbps;

    // Get the directory to setup the cache, or use the default one. The default cache directory is
    // named "cache-<name>", where name is the last component of the bucket name's path. We'll
    // create it if it doesn't exist.
    WT_CONFIG_ITEM cacheDirConf;
    std::string cacheDir;
    std::string cacheStr;
    if ((ret = s3->wtApi->config_get_string(
           s3->wtApi, session, config, "cache_directory", &cacheDirConf)) == 0)
        cacheStr = std::string(cacheDirConf.str, cacheDirConf.len);
    else if (ret == WT_NOTFOUND) {
        cacheStr = "cache-" + bucketName;
        ret = 0;
    } else {
        s3->log->LogErrorMessage(
          "S3CustomizeFileSystem: error parsing config for cache directory.");
        return (ret);
    }

    // Fetch the native WT file system.
    WT_FILE_SYSTEM *wtFileSystem;
    if ((ret = s3->wtApi->file_system_get(s3->wtApi, session, &wtFileSystem)) != 0)
        return (ret);

    // Get a copy of the home and cache directory.
    const std::string homeDir = session->connection->get_home(session->connection);
    if ((ret = S3GetDirectory(*s3, homeDir, cacheStr, true, cacheDir)) != 0)
        return (ret);

    // Create the file system.
    S3FileSystem *fs;
    if ((fs = (S3FileSystem *)calloc(1, sizeof(S3FileSystem))) == nullptr) {
        s3->log->LogErrorMessage(
          "S3CustomizeFileSystem: unable to allocate memory for file system.");
        return (ENOMEM);
    }
    fs->storage = s3;
    fs->wtFileSystem = wtFileSystem;
    fs->homeDir = homeDir;
    fs->cacheDir = cacheDir;

    try {
        fs->connection = new S3Connection(credentials, awsConfig, bucketName, objPrefix);
    } catch (std::invalid_argument &e) {
        s3->log->LogErrorMessage(std::string("S3CustomizeFileSystem: ") + e.what());
        return (EINVAL);
    }
    fs->fileSystem.fs_directory_list = S3ObjectList;
    fs->fileSystem.fs_directory_list_single = S3ObjectListSingle;
    fs->fileSystem.fs_directory_list_free = S3ObjectListFree;
    fs->fileSystem.terminate = S3FileSystemTerminate;
    fs->fileSystem.fs_exist = S3FileExists;
    fs->fileSystem.fs_open_file = S3FileOpen;
    fs->fileSystem.fs_remove = S3Remove;
    fs->fileSystem.fs_rename = S3Rename;
    fs->fileSystem.fs_size = S3ObjectSize;

    s3->log->LogDebugMessage("S3CustomizeFileSystem: S3 connection established.");

    // Add to the list of the active file systems. Lock will be freed when the scope is exited.
    {
        std::lock_guard<std::mutex> lockGuard(s3->fsListMutex);
        s3->fsList.push_back(fs);
    }

    *fileSystem = &fs->fileSystem;
    return (ret);
}

// Discard any resources on termination of the file system.
static int
S3FileSystemTerminate(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session)
{
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    S3Storage *s3 = FS2S3(fileSystem);

    UNUSED(session);

    // Remove from the active filesystems list. The lock will be freed when the scope is exited.
    {
        std::lock_guard<std::mutex> lockGuard(s3->fsListMutex);
        s3->fsList.remove(fs);
    }
    delete (fs->connection);
    free(fs);

    return (0);
}

// Return a list of object names for the given location.
static int
S3ObjectListInternal(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *directory,
  const char *prefix, char ***objectList, uint32_t *count, bool listSingle)
{
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    S3Storage *s3 = FS2S3(fileSystem);
    std::vector<std::string> objects;
    std::string completePrefix;

    *count = 0;

    if (directory != nullptr) {
        completePrefix += directory;
        // Add a terminating '/' if one doesn't exist.
        if (completePrefix.length() > 1 && completePrefix[completePrefix.length() - 1] != '/')
            completePrefix += '/';
    }
    if (prefix != nullptr)
        completePrefix += prefix;

    int ret;
    s3->statistics.listObjectsCount++;

    ret = listSingle ? fs->connection->ListObjects(completePrefix, objects, 1, true) :
                       fs->connection->ListObjects(completePrefix, objects);

    if (ret != 0) {
        s3->log->LogErrorMessage("S3ObjectList: ListObjects request to S3 failed.");
        return (ret);
    }
    *count = objects.size();

    s3->log->LogDebugMessage("S3ObjectList: ListObjects request to S3 succeeded. Received " +
      std::to_string(*count) + " objects.");
    S3ObjectListAdd(*s3, objectList, objects, *count);

    return (ret);
}

// Return a list of object names for the given location.
static int
S3ObjectList(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *directory,
  const char *prefix, char ***objectList, uint32_t *count)
{
    return (S3ObjectListInternal(fileSystem, session, directory, prefix, objectList, count, false));
}

// Return a single object name for the given location.
static int
S3ObjectListSingle(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *directory,
  const char *prefix, char ***objectList, uint32_t *count)
{
    return (S3ObjectListInternal(fileSystem, session, directory, prefix, objectList, count, true));
}

// Free memory allocated by S3ObjectList.
static int
S3ObjectListFree(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, char **objectList, uint32_t count)
{
    UNUSED(fileSystem);
    UNUSED(session);

    if (objectList != nullptr) {
        while (count > 0)
            free(objectList[--count]);
        free(objectList);
    }

    return (0);
}

// Add objects retrieved from S3 bucket into the object list, and allocate the memory needed.
static int
S3ObjectListAdd(const S3Storage &s3, char ***objectList, const std::vector<std::string> &objects,
  const uint32_t count)
{
    char **entries;
    if ((entries = (char **)malloc(sizeof(char *) * count)) == nullptr) {
        s3.log->LogErrorMessage("S3ObjectListAdd: unable to allocate memory for object list.");
        return (ENOMEM);
    }

    for (int i = 0; i < count; i++) {
        if ((entries[i] = strdup(objects[i].c_str())) == nullptr) {
            s3.log->LogErrorMessage(
              "S3ObjectListAdd: unable to allocate memory for object string.");
            return (ENOMEM);
        }
    }
    *objectList = entries;

    return (0);
}

// Add a reference to the storage source so we can reference count to know when to really
// terminate.
static int
S3AddReference(WT_STORAGE_SOURCE *storageSource)
{
    S3Storage *s3 = (S3Storage *)storageSource;

    if (s3->referenceCount == 0 || s3->referenceCount + 1 == 0) {
        s3->log->LogErrorMessage("S3AddReference: missing reference or overflow.");
        return (EINVAL);
    }

    ++s3->referenceCount;
    return (0);
}

// Discard any resources on termination.

static int
S3Terminate(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session)
{
    S3Storage *s3 = (S3Storage *)storageSource;

    if (--s3->referenceCount != 0)
        return (0);

    // It is currently unclear at the moment what the multi-threading will look like in the
    // extension. The current implementation is NOT thread-safe, and needs to be addressed in the
    // future, as multiple threads could call terminate leading to a race condition.
    while (!s3->fhList.empty()) {
        S3FileHandle *fs = s3->fhList.front();
        S3FileClose((WT_FILE_HANDLE *)fs, session);
    }

    // Terminate any active filesystems. There are no references to the storage source, so it is
    // safe to walk the active filesystem list without a lock. The removal from the list happens
    // under a lock. Also, removal happens from the front and addition at the end, so we are safe.
    while (!s3->fsList.empty()) {
        S3FileSystem *fs = s3->fsList.front();
        S3FileSystemTerminate(&fs->fileSystem, session);
    }

    S3LogStatistics(*s3);

    Aws::Utils::Logging::ShutdownAWSLogging();
    AwsManager::Terminate();

    s3->log->LogDebugMessage("S3Terminate: Terminated S3 storage source.");
    delete (s3);
    return (0);
}

// Flush file to S3 Store using AWS SDK C++ PutObject.
static int
S3Flush(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session, WT_FILE_SYSTEM *fileSystem,
  const char *source, const char *object, const char *config)
{
    S3Storage *s3 = (S3Storage *)storageSource;
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    WT_FILE_SYSTEM *wtFileSystem = fs->wtFileSystem;
    FS2S3(fileSystem)->statistics.putObjectCount++;

    // Confirm that the file exists on the native filesystem.
    std::string srcPath = S3Path(fs->homeDir, source);
    bool nativeExist = false;
    int ret = wtFileSystem->fs_exist(wtFileSystem, session, srcPath.c_str(), &nativeExist);
    if (ret != 0) {
        s3->log->LogErrorMessage("S3Flush: Failed to check for the existence of " +
          std::string(source) + " on the native filesystem.");
        return (ret);
    }
    if (!nativeExist) {
        s3->log->LogErrorMessage("S3Flush: " + std::string(source) + " No such file.");
        return (ENOENT);
    }

    s3->log->LogDebugMessage(
      "S3Flush: Uploading object: " + std::string(object) + "into bucket using PutObject");
    // Upload the object into the bucket.
    ret = (fs->connection->PutObject(object, srcPath));
    if (ret != 0)
        s3->log->LogErrorMessage("S3Flush: PutObject request to S3 failed.");
    else
        s3->log->LogDebugMessage("S3Flush: Uploaded object to S3.");

    return (ret);
}

// Flush local file to cache.
static int
S3FlushFinish(WT_STORAGE_SOURCE *storage, WT_SESSION *session, WT_FILE_SYSTEM *fileSystem,
  const char *source, const char *object, const char *config)
{
    S3Storage *s3 = (S3Storage *)storage;
    S3FileSystem *fs = (S3FileSystem *)fileSystem;
    // Constructing the pathname for source and cache from file system and local.
    std::string srcPath = S3Path(fs->homeDir, source);
    std::string destPath = S3Path(fs->cacheDir, object);

    // Converting S3 object name to cache directory strcture to link the cache file with local file.
    std::filesystem::create_directories(std::filesystem::path(destPath).parent_path());

    // Linking file with the local file.
    std::error_code ec;
    std::filesystem::create_hard_link(srcPath.c_str(), destPath.c_str(), ec);
    int ret = ec.value();
    if (ret != 0) {
        ret = errno;
        s3->log->LogErrorMessage(
          "S3FlushFinish: link " + std::string(source) + " to " + destPath + " failed");
        return (ret);
    }

    // The file should be read-only.
    std::filesystem::permissions(destPath.c_str(),
      std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
        std::filesystem::perms::others_read,
      std::filesystem::perm_options::add, ec);
    ret = ec.value();
    if (ret != 0) {
        ret = errno;
        s3->log->LogErrorMessage("S3FlushFinish: read permissions of " + destPath + " failed");
    }
    return (ret);
}

// Log collected statistics.
static void
S3LogStatistics(const S3Storage &s3)
{
    s3.log->LogDebugMessage(
      "S3 list objects count: " + std::to_string(s3.statistics.listObjectsCount));
    s3.log->LogDebugMessage("S3 put object count: " + std::to_string(s3.statistics.putObjectCount));
    s3.log->LogDebugMessage("S3 get object count: " + std::to_string(s3.statistics.getObjectCount));
    s3.log->LogDebugMessage(
      "S3 object exists count: " + std::to_string(s3.statistics.objectExistsCount));

    s3.log->LogDebugMessage(
      "Non read/write file handle operations: " + std::to_string(s3.statistics.fhOps));
    s3.log->LogDebugMessage(
      "File handle read operations: " + std::to_string(s3.statistics.fhReadOps));
}

// A S3 storage source library.
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    S3Storage *s3;
    WT_CONFIG_ITEM v;

    s3 = new S3Storage;
    s3->wtApi = connection->get_extension_api(connection);
    int ret = s3->wtApi->config_get(s3->wtApi, nullptr, config, "verbose.tiered", &v);

    // Create a logger for the storage source. Verbose level defaults to WT_VERBOSE_ERROR (-3) if it
    // is outside the valid range or not found.
    s3->verbose = WT_VERBOSE_ERROR;
    s3->log = Aws::MakeShared<S3LogSystem>("storage", s3->wtApi, s3->verbose);

    if (ret == 0 && v.val >= WT_VERBOSE_ERROR && v.val <= WT_VERBOSE_DEBUG) {
        s3->verbose = v.val;
        s3->log->SetWtVerbosityLevel(s3->verbose);
    } else if (ret != WT_NOTFOUND) {
        s3->log->LogErrorMessage(
          "wiredtiger_extension_init: error parsing config for verbose level.");
        delete (s3);
        return (ret != 0 ? ret : EINVAL);
    }

    // Set up statistics.
    s3->statistics = {};

    // Initialize the AWS SDK and logging.
    AwsManager::Init();
    std::shared_ptr<Aws::Utils::Logging::LogSystemInterface> s3Log = s3->log;
    Aws::Utils::Logging::InitializeAWSLogging(s3Log);

    // Allocate a S3 storage structure, with a WT_STORAGE structure as the first field, allowing us
    // to treat references to either type of structure as a reference to the other type.
    s3->storageSource.ss_customize_file_system = S3CustomizeFileSystem;
    s3->storageSource.ss_add_reference = S3AddReference;
    s3->storageSource.terminate = S3Terminate;
    s3->storageSource.ss_flush = S3Flush;
    s3->storageSource.ss_flush_finish = S3FlushFinish;

    // The first reference is implied by the call to add_storage_source.
    s3->referenceCount = 1;

    // Load the storage
    if ((ret = connection->add_storage_source(
           connection, "s3_store", &s3->storageSource, nullptr)) != 0) {
        s3->log->LogErrorMessage(
          "wiredtiger_extension_init: Could not load S3 storage source, shutting down.");
        Aws::Utils::Logging::ShutdownAWSLogging();
        AwsManager::Terminate();
        delete (s3);
    }

    return (ret);
}
