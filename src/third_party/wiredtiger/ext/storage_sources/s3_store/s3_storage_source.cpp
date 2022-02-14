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
#include <sys/stat.h>
#include <fstream>
#include <list>
#include <errno.h>
#include <unistd.h>

#include "s3_connection.h"
#include "s3_log_system.h"
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/logging/AWSLogging.h>

#define UNUSED(x) (void)(x)
#define FS2S3(fs) (((S3_FILE_SYSTEM *)(fs))->storage)

struct S3_FILE_HANDLE;
struct S3_FILE_SYSTEM;

/* Statistics to be collected for the S3 storage. */
struct S3_STATISTICS {
    /* Operations using AWS SDK. */
    uint64_t listObjectsCount;  /* Number of S3 list objects requests */
    uint64_t putObjectCount;    /* Number of S3 put object requests */
    uint64_t getObjectCount;    /* Number of S3 get object requests */
    uint64_t objectExistsCount; /* Number of S3 object exists requests */

    /* Operations using WiredTiger's native file handle operations. */
    uint64_t fhOps;     /* Number of non read/write file handle operations */
    uint64_t fhReadOps; /* Number of file handle read operations */
};

/* S3 storage source structure. */
struct S3_STORAGE {
    WT_STORAGE_SOURCE storageSource; /* Must come first */
    WT_EXTENSION_API *wtApi;         /* Extension API */

    std::mutex fsListMutex;             /* Protect the file system list */
    std::list<S3_FILE_SYSTEM *> fsList; /* List of initiated file systems */
    std::mutex fhMutex;                 /* Protect the file handle list*/
    std::list<S3_FILE_HANDLE *> fhList; /* List of open file handles */

    uint32_t referenceCount; /* Number of references to this storage source */
    int32_t verbose;

    S3_STATISTICS statistics;
};

struct S3_FILE_SYSTEM {
    /* Must come first - this is the interface for the file system we are implementing. */
    WT_FILE_SYSTEM fileSystem;
    S3_STORAGE *storage;
    /*
     * The S3_FILE_SYSTEM is built on top of the WT_FILE_SYSTEM. We require an instance of the
     * WT_FILE_SYSTEM in order to access the native WiredTiger filesystem functionality, such as the
     * native WT file handle open.
     */
    WT_FILE_SYSTEM *wtFileSystem;
    S3Connection *connection;
    S3LogSystem *log;
    std::string cacheDir; /* Directory for cached objects */
    std::string homeDir;  /* Owned by the connection */
};

struct S3_FILE_HANDLE {
    WT_FILE_HANDLE iface; /* Must come first */
    S3_STORAGE *storage;  /* Enclosing storage source */
    /*
     * Similarly, The S3_FILE_HANDLE is built on top of the WT_FILE_HANDLE. We require an instance
     * of the WT_FILE_HANDLE in order to access the native WiredTiger filehandle functionality, such
     * as the native WT file handle read and close.
     */
    WT_FILE_HANDLE *wtFileHandle;
};

/* Configuration variables for connecting to S3CrtClient. */
const Aws::String region = Aws::Region::AP_SOUTHEAST_2;
const double throughputTargetGbps = 5;
const uint64_t partSize = 8 * 1024 * 1024; /* 8 MB. */

/* Setting SDK options. */
Aws::SDKOptions options;

static int S3GetDirectory(const std::string &, const std::string &, bool, std::string &);
static bool S3CacheExists(WT_FILE_SYSTEM *, const std::string &);
static std::string S3Path(const std::string &, const std::string &);
static std::string S3HomePath(WT_FILE_SYSTEM *, const char *);
static std::string S3CachePath(WT_FILE_SYSTEM *, const char *);
static int S3Exist(WT_FILE_SYSTEM *, WT_SESSION *, const char *, bool *);
static int S3CustomizeFileSystem(
  WT_STORAGE_SOURCE *, WT_SESSION *, const char *, const char *, const char *, WT_FILE_SYSTEM **);
static int S3AddReference(WT_STORAGE_SOURCE *);
static int S3FileSystemTerminate(WT_FILE_SYSTEM *, WT_SESSION *);
static int S3Open(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, WT_FS_OPEN_FILE_TYPE, uint32_t, WT_FILE_HANDLE **);
static bool LocalFileExists(const std::string &);
static int S3FileRead(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t, size_t, void *);
static int S3ObjectList(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListAdd(
  S3_STORAGE *, char ***, const std::vector<std::string> &, const uint32_t);
static int S3ObjectListSingle(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListFree(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);
static void S3ShowStatistics(const S3_STATISTICS &);

static int S3FileClose(WT_FILE_HANDLE *, WT_SESSION *);
/*
 * S3Path --
 *     Construct a pathname from the directory and the object name.
 */
static std::string
S3Path(const std::string &dir, const std::string &name)
{
    /* Skip over "./" and variations (".//", ".///./././//") at the beginning of the name. */
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

/*
 *   S3Exist--
 *     Return if the file exists. First checks the cache, and then the S3 Bucket.
 */
static int
S3Exist(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *name, bool *exist)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    int ret = 0;

    /* Check if file exists in the cache. */
    *exist = S3CacheExists(fileSystem, name);
    if (*exist)
        return (ret);

    /* It's not in the cache, try the S3 bucket. */
    FS2S3(fileSystem)->statistics.objectExistsCount++;
    if ((ret = fs->connection->ObjectExists(name, *exist)) != 0)
        std::cerr << "S3Exist: ObjectExists request to S3 failed." << std::endl;
    return (ret);
}

/*
 * S3CacheExists --
 *     Checks whether the given file exists in the cache.
 */
static bool
S3CacheExists(WT_FILE_SYSTEM *fileSystem, const std::string &name)
{
    const std::string path = S3Path(((S3_FILE_SYSTEM *)fileSystem)->cacheDir, name);
    return (LocalFileExists(path));
}

/*
 * LocalFileExists --
 *     Checks whether a file corresponding to the provided path exists locally.
 */
static bool
LocalFileExists(const std::string &path)
{
    std::ifstream f(path);
    return (f.good());
}

/*
 * S3GetDirectory --
 *     Return a copy of a directory name after verifying that it is a directory.
 */
static int
S3GetDirectory(const std::string &home, const std::string &name, bool create, std::string &copy)
{
    copy = "";

    struct stat sb;
    int ret;
    std::string dirName;

    /* For relative pathnames, the path is considered to be relative to the home directory. */
    if (name[0] == '/')
        dirName = name;
    else
        dirName = home + "/" + name;

    ret = stat(dirName.c_str(), &sb);
    if (ret != 0 && errno == ENOENT && create) {
        (void)mkdir(dirName.c_str(), 0777);
        ret = stat(dirName.c_str(), &sb);
    }

    if (ret != 0)
        ret = errno;
    else if ((sb.st_mode & S_IFMT) != S_IFDIR)
        ret = EINVAL;

    copy = dirName;
    return (ret);
}

/*
 * S3FileClose --
 *    File handle close.
 */
static int
S3FileClose(WT_FILE_HANDLE *fileHandle, WT_SESSION *session)
{
    int ret = 0;
    S3_FILE_HANDLE *s3FileHandle = (S3_FILE_HANDLE *)fileHandle;
    S3_STORAGE *storage = s3FileHandle->storage;
    WT_FILE_HANDLE *wtFileHandle = s3FileHandle->wtFileHandle;
    /*
     * We require exclusive access to the list of file handles when removing file handles. The
     * lock_guard will be unlocked automatically once the scope is exited.
     */
    {
        std::lock_guard<std::mutex> lock(storage->fhMutex);
        storage->fhList.remove(s3FileHandle);
    }
    if (wtFileHandle != NULL) {
        storage->statistics.fhOps++;
        ret = wtFileHandle->close(wtFileHandle, session);
    }

    free(s3FileHandle->iface.name);
    free(s3FileHandle);

    return (ret);
}

/*
 * S3Open --
 *    File open for the s3 storage source.
 */
static int
S3Open(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *name,
  WT_FS_OPEN_FILE_TYPE fileType, uint32_t flags, WT_FILE_HANDLE **fileHandlePtr)
{
    S3_FILE_HANDLE *s3FileHandle;
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    S3_STORAGE *s3 = fs->storage;
    WT_FILE_SYSTEM *wtFileSystem = fs->wtFileSystem;
    WT_FILE_HANDLE *wtFileHandle;
    int ret;

    *fileHandlePtr = NULL;

    /* We only support opening the file in read only mode. */
    if ((flags & WT_FS_OPEN_READONLY) == 0 || (flags & WT_FS_OPEN_CREATE) != 0) {
        std::cerr << "ss_open_object: readonly access required: " << name << std::endl;
        return (EINVAL);
    }

    /*
     * Currently, only data files should be being opened; although this constraint can be relaxed in
     * the future.
     */
    if (fileType != WT_FS_OPEN_FILE_TYPE_DATA && fileType != WT_FS_OPEN_FILE_TYPE_REGULAR) {
        std::cerr << name << ": open: only data file and regular types supported" << std::endl;
        return (EINVAL);
    }

    if ((s3FileHandle = (S3_FILE_HANDLE *)calloc(1, sizeof(S3_FILE_HANDLE))) == NULL)
        return (ENOMEM);

    /* Make a copy from S3 if the file is not in the cache. */
    const std::string cachePath = S3Path(fs->cacheDir, name);
    if (!LocalFileExists(cachePath)) {
        s3->statistics.getObjectCount++;
        if ((ret = fs->connection->GetObject(name, cachePath)) != 0) {
            std::cerr << "ss_open_object: GetObject request to S3 failed." << std::endl;
            return (ret);
        }
    }

    /* Use WiredTiger's native file handle open. */
    ret = wtFileSystem->fs_open_file(
      wtFileSystem, session, cachePath.c_str(), fileType, flags, &wtFileHandle);
    if (ret != 0) {
        std::cerr << "ss_open_object: fs_open_file failed." << name << std::endl;
        return (ret);
    }

    s3FileHandle->wtFileHandle = wtFileHandle;
    s3FileHandle->storage = s3;

    WT_FILE_HANDLE *fileHandle = (WT_FILE_HANDLE *)s3FileHandle;
    fileHandle->close = S3FileClose;
    fileHandle->fh_advise = NULL;
    fileHandle->fh_extend = NULL;
    fileHandle->fh_extend_nolock = NULL;
    fileHandle->fh_lock = NULL;
    fileHandle->fh_map = NULL;
    fileHandle->fh_map_discard = NULL;
    fileHandle->fh_map_preload = NULL;
    fileHandle->fh_unmap = NULL;
    fileHandle->fh_read = S3FileRead;
    fileHandle->fh_size = NULL;
    fileHandle->fh_sync = NULL;
    fileHandle->fh_sync_nowait = NULL;
    fileHandle->fh_truncate = NULL;
    fileHandle->fh_write = NULL;

    fileHandle->name = strdup(name);
    if (fileHandle->name == NULL) {
        std::cout << "ss_open_object: unable to allocate memory for object name" << std::endl;
        return (ENOMEM);
    }

    /*
     * We require exclusive access to the list of file handles when adding file handles to it. The
     * lock_guard will be unlocked automatically when the scope is exited.
     */
    {
        std::lock_guard<std::mutex> lock(s3->fhMutex);
        s3FileHandle->storage->fhList.push_back(s3FileHandle);
    }

    *fileHandlePtr = fileHandle;
    return (0);
}

/*
 * S3FileRead --
 *     Read a file using WiredTiger's native file handle read.
 */
static int
S3FileRead(WT_FILE_HANDLE *fileHandle, WT_SESSION *session, wt_off_t offset, size_t len, void *buf)
{
    S3_FILE_HANDLE *s3FileHandle = (S3_FILE_HANDLE *)fileHandle;
    S3_STORAGE *storage = s3FileHandle->storage;
    WT_FILE_HANDLE *wtFileHandle = s3FileHandle->wtFileHandle;
    int ret;
    storage->statistics.fhReadOps++;
    if ((ret = wtFileHandle->fh_read(wtFileHandle, session, offset, len, buf)) != 0)
        std::cerr << "S3FileRead: fh_read failed." << std::endl;
    return (ret);
}

/*
 * S3CustomizeFileSystem --
 *     Return a customized file system to access the s3 storage source objects.
 */
static int
S3CustomizeFileSystem(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session, const char *bucketName,
  const char *authToken, const char *config, WT_FILE_SYSTEM **fileSystem)
{
    S3_FILE_SYSTEM *fs;
    WT_FILE_SYSTEM *wtFileSystem;
    S3_STORAGE *s3;
    int ret;
    std::string cacheDir;

    s3 = (S3_STORAGE *)storageSource;

    /* Mark parameters as unused for now, until implemented. */
    UNUSED(authToken);

    /* We need to have a bucket to setup the file system. */
    if (bucketName == NULL || strlen(bucketName) == 0) {
        std::cerr << "Error: Bucket not specified";
        return (EINVAL);
    }

    /*
     * Parse configuration string.
     */

    /* Get any prefix to be used for the object keys. */
    WT_CONFIG_ITEM objPrefixConf;
    std::string objPrefix;
    if ((ret = s3->wtApi->config_get_string(
           s3->wtApi, session, config, "prefix", &objPrefixConf)) == 0)
        objPrefix = objPrefixConf.str;
    else if (ret != WT_NOTFOUND) {
        std::cerr << "Error: customize_file_system: config parsing for object prefix";
        return (1);
    }

    /*
     * Get the directory to setup the cache, or use the default one. The default cache directory is
     * named "cache-<name>", where name is the last component of the bucket name's path. We'll
     * create it if it doesn't exist.
     */
    WT_CONFIG_ITEM cacheDirConf;
    std::string cacheStr;
    if ((ret = s3->wtApi->config_get_string(
           s3->wtApi, session, config, "cache_directory", &cacheDirConf)) == 0)
        cacheStr = cacheDirConf.str;
    else if (ret == WT_NOTFOUND) {
        cacheStr = "cache-" + std::string(bucketName);
        ret = 0;
    } else
        return (ret);

    /* Fetch the native WT file system. */
    if ((ret = s3->wtApi->file_system_get(s3->wtApi, session, &wtFileSystem)) != 0)
        return (ret);

    /* Get a copy of the home and cache directory. */
    const std::string homeDir = session->connection->get_home(session->connection);
    if ((ret = S3GetDirectory(homeDir, cacheStr, true, cacheDir)) != 0)
        return (ret);

    /* Create the file system. */
    if ((fs = (S3_FILE_SYSTEM *)calloc(1, sizeof(S3_FILE_SYSTEM))) == NULL)
        return (errno);
    fs->storage = s3;
    fs->wtFileSystem = wtFileSystem;
    fs->homeDir = homeDir;
    fs->cacheDir = cacheDir;

    Aws::S3Crt::ClientConfiguration awsConfig;
    awsConfig.region = region;
    awsConfig.throughputTargetGbps = throughputTargetGbps;
    awsConfig.partSize = partSize;

    /* New can fail; will deal with this later. */
    fs->connection = new S3Connection(awsConfig, bucketName, objPrefix);
    fs->fileSystem.fs_directory_list = S3ObjectList;
    fs->fileSystem.fs_directory_list_single = S3ObjectListSingle;
    fs->fileSystem.fs_directory_list_free = S3ObjectListFree;
    fs->fileSystem.terminate = S3FileSystemTerminate;
    fs->fileSystem.fs_exist = S3Exist;
    fs->fileSystem.fs_open_file = S3Open;

    /* Add to the list of the active file systems. Lock will be freed when the scope is exited. */
    {
        std::lock_guard<std::mutex> lockGuard(s3->fsListMutex);
        s3->fsList.push_back(fs);
    }

    *fileSystem = &fs->fileSystem;
    return (ret);
}

/*
 * S3FileSystemTerminate --
 *     Discard any resources on termination of the file system.
 */
static int
S3FileSystemTerminate(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    S3_STORAGE *s3 = fs->storage;

    UNUSED(session); /* unused */

    /* Remove from the active filesystems list. The lock will be freed when the scope is exited. */
    {
        std::lock_guard<std::mutex> lockGuard(s3->fsListMutex);
        s3->fsList.remove(fs);
    }
    delete (fs->connection);
    free(fs);

    return (0);
}

/*
 * S3ObjectList --
 *     Return a list of object names for the given location.
 */
static int
S3ObjectList(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *directory,
  const char *prefix, char ***objectList, uint32_t *count)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    S3_STORAGE *s3 = FS2S3(fileSystem);

    std::vector<std::string> objects;
    std::string completePrefix;

    if (directory != NULL) {
        completePrefix += directory;
        /* Add a terminating '/' if one doesn't exist. */
        if (completePrefix.length() > 1 && completePrefix[completePrefix.length() - 1] != '/')
            completePrefix += '/';
    }
    if (prefix != NULL)
        completePrefix += prefix;

    int ret;
    s3->statistics.listObjectsCount++;
    if ((ret = fs->connection->ListObjects(completePrefix, objects)) != 0) {
        std::cerr << "S3ObjectList: ListObjects request to S3 failed." << std::endl;
        return (ret);
    }
    *count = objects.size();

    S3ObjectListAdd(s3, objectList, objects, *count);

    return (ret);
}

/*
 * S3ObjectListSingle --
 *     Return a single object name for the given location.
 */
static int
S3ObjectListSingle(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, const char *directory,
  const char *prefix, char ***objectList, uint32_t *count)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    S3_STORAGE *s3 = FS2S3(fileSystem);

    std::vector<std::string> objects;
    std::string completePrefix;

    if (directory != NULL) {
        completePrefix += directory;
        /* Add a terminating '/' if one doesn't exist. */
        if (completePrefix.length() > 1 && completePrefix[completePrefix.length() - 1] != '/')
            completePrefix += '/';
    }
    if (prefix != NULL)
        completePrefix += prefix;

    int ret;
    s3->statistics.listObjectsCount++;
    if ((ret = fs->connection->ListObjects(completePrefix, objects, 1, true)) != 0) {
        std::cerr << "S3ObjectListSingle: ListObjects request to S3 failed." << std::endl;
        return (ret);
    }

    *count = objects.size();

    S3ObjectListAdd(s3, objectList, objects, *count);

    return (ret);
}

/*
 * S3ObjectListFree --
 *     Free memory allocated by S3ObjectList.
 */
static int
S3ObjectListFree(WT_FILE_SYSTEM *fileSystem, WT_SESSION *session, char **objectList, uint32_t count)
{
    (void)fileSystem;
    (void)session;

    if (objectList != NULL) {
        while (count > 0)
            free(objectList[--count]);
        free(objectList);
    }

    return (0);
}

/*
 * S3ObjectListAdd --
 *     Add objects retrieved from S3 bucket into the object list, and allocate the memory needed.
 */
static int
S3ObjectListAdd(
  S3_STORAGE *s3, char ***objectList, const std::vector<std::string> &objects, const uint32_t count)
{
    char **entries = (char **)malloc(sizeof(char *) * count);
    for (int i = 0; i < count; i++) {
        entries[i] = strdup(objects[i].c_str());
    }
    *objectList = entries;

    return (0);
}

/*
 * S3AddReference --
 *     Add a reference to the storage source so we can reference count to know when to really
 *     terminate.
 */
static int
S3AddReference(WT_STORAGE_SOURCE *storageSource)
{
    S3_STORAGE *s3 = (S3_STORAGE *)storageSource;

    /*
     * Missing reference or overflow?
     */
    if (s3->referenceCount == 0 || s3->referenceCount + 1 == 0)
        return (EINVAL);

    ++s3->referenceCount;
    return (0);
}

/*
 * S3Terminate --
 *     Discard any resources on termination.
 */
static int
S3Terminate(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session)
{
    S3_STORAGE *s3 = (S3_STORAGE *)storageSource;

    if (--s3->referenceCount != 0)
        return (0);

    /*
     * Is it currently unclear at the moment what the multi-threading will look like in the
     * extension. The current implementation is NOT thread-safe, and needs to be addressed in the
     * future, as mulitple threads could call terminate leading to a race condition.
     */
    while (!s3->fhList.empty()) {
        S3_FILE_HANDLE *fs = s3->fhList.front();
        S3FileClose((WT_FILE_HANDLE *)fs, session);
    }
    /*
     * Terminate any active filesystems. There are no references to the storage source, so it is
     * safe to walk the active filesystem list without a lock. The removal from the list happens
     * under a lock. Also, removal happens from the front and addition at the end, so we are safe.
     */
    while (!s3->fsList.empty()) {
        S3_FILE_SYSTEM *fs = s3->fsList.front();
        S3FileSystemTerminate(&fs->fileSystem, session);
    }

    /* Log collected statistics on termination. */
    S3ShowStatistics(s3->statistics);

    Aws::Utils::Logging::ShutdownAWSLogging();
    Aws::ShutdownAPI(options);

    delete (s3);
    return (0);
}

/*
 * S3Flush --
 *     Flush file to S3 Store using AWS SDK C++ PutObject.
 */
static int
S3Flush(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session, WT_FILE_SYSTEM *fileSystem,
  const char *source, const char *object, const char *config)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;

    int ret;
    FS2S3(fileSystem)->statistics.putObjectCount++;
    if (ret = (fs->connection->PutObject(object, source)) != 0)
        std::cerr << "S3Flush: PutObject request to S3 failed." << std::endl;
    return (ret);
}

/*
 * S3FlushFinish --
 *     Flush local file to cache.
 */
static int
S3FlushFinish(WT_STORAGE_SOURCE *storage, WT_SESSION *session, WT_FILE_SYSTEM *fileSystem,
  const char *source, const char *object, const char *config)
{
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;
    /* Constructing the pathname for source and cache from file system and local.  */
    std::string srcPath = S3Path(fs->homeDir, source);
    std::string destPath = S3Path(fs->cacheDir, source);

    /* Linking file with the local file. */
    int ret = link(srcPath.c_str(), destPath.c_str());

    /* The file should be read-only. */
    if (ret == 0)
        ret = chmod(destPath.c_str(), 0444);
    return (ret);
}

/*
 * S3ShowStatistics --
 *     Log collected statistics.
 */
static void
S3ShowStatistics(const S3_STATISTICS &statistics)
{
    std::cout << "S3 list objects count: " << statistics.listObjectsCount << std::endl;
    std::cout << "S3 put object count: " << statistics.putObjectCount << std::endl;
    std::cout << "S3 get object count: " << statistics.putObjectCount << std::endl;
    std::cout << "S3 object exists count: " << statistics.objectExistsCount << std::endl;

    std::cout << "Non read/write file handle operations: " << statistics.fhOps << std::endl;
    std::cout << "File handle read operations: " << statistics.fhReadOps << std::endl;
}

/*
 * wiredtiger_extension_init --
 *     A S3 storage source library.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    S3_STORAGE *s3;
    S3_FILE_SYSTEM *fs;
    WT_CONFIG_ITEM v;

    s3 = new S3_STORAGE;

    s3->wtApi = connection->get_extension_api(connection);

    int ret = s3->wtApi->config_get(s3->wtApi, NULL, config, "verbose", &v);

    // If a verbose level is not found, it will set the level to -3 (Error).
    if (ret == 0 && v.val >= -3 && v.val <= 1)
        s3->verbose = v.val;
    else if (ret == WT_NOTFOUND)
        s3->verbose = -3;
    else {
        free(s3);
        return (ret != 0 ? ret : EINVAL);
    }

    /* Set up statistics. */
    s3->statistics = {0};

    /* Create a logger for this storage source, and then initialize the AWS SDK. */
    Aws::Utils::Logging::InitializeAWSLogging(
      Aws::MakeShared<S3LogSystem>("storage", s3->wtApi, s3->verbose));
    Aws::InitAPI(options);

    /*
     * Allocate a S3 storage structure, with a WT_STORAGE structure as the first field, allowing us
     * to treat references to either type of structure as a reference to the other type.
     */
    s3->storageSource.ss_customize_file_system = S3CustomizeFileSystem;
    s3->storageSource.ss_add_reference = S3AddReference;
    s3->storageSource.terminate = S3Terminate;
    s3->storageSource.ss_flush = S3Flush;
    s3->storageSource.ss_flush_finish = S3FlushFinish;

    /*
     * The first reference is implied by the call to add_storage_source.
     */
    s3->referenceCount = 1;

    /* Load the storage */
    if ((ret = connection->add_storage_source(connection, "s3_store", &s3->storageSource, NULL)) !=
      0)
        free(s3);

    return (ret);
}
