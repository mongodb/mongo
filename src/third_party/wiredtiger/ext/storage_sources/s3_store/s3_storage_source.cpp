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

struct S3_FILE_SYSTEM;

/* S3 storage source structure. */
struct S3_STORAGE {
    WT_STORAGE_SOURCE storageSource; /* Must come first */
    WT_EXTENSION_API *wtApi;         /* Extension API */

    std::mutex fsListMutex;             /* Protect the file system list */
    std::list<S3_FILE_SYSTEM *> fsList; /* List of initiated file systems */

    uint32_t referenceCount; /* Number of references to this storge source */
    int32_t verbose;
};

struct S3_FILE_SYSTEM {
    /* Must come first - this is the interface for the file system we are implementing. */
    WT_FILE_SYSTEM fileSystem;
    S3Connection *connection;
    S3LogSystem *log;
    S3_STORAGE *storage;
    std::string cacheDir; /* Directory for cached objects */
    std::string homeDir;  /* Owned by the connection */
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

static int S3ObjectList(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListAdd(
  S3_STORAGE *, char ***, const std::vector<std::string> &, const uint32_t);
static int S3ObjectListSingle(
  WT_FILE_SYSTEM *, WT_SESSION *, const char *, const char *, char ***, uint32_t *);
static int S3ObjectListFree(WT_FILE_SYSTEM *, WT_SESSION *, char **, uint32_t);

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
    S3_STORAGE *s3;
    s3 = FS2S3(fileSystem);
    S3_FILE_SYSTEM *fs = (S3_FILE_SYSTEM *)fileSystem;

    /* It's not in the cache, try the S3 bucket. */
    *exist = S3CacheExists(fileSystem, name);
    if (!*exist)
        return (fs->connection->ObjectExists(name, *exist));

    return (0);
}

/*
 * S3CacheExists --
 *     Checks whether the given file exists in the cache.
 */
static bool
S3CacheExists(WT_FILE_SYSTEM *fileSystem, const std::string &name)
{
    std::string path = S3Path(((S3_FILE_SYSTEM *)fileSystem)->cacheDir, name);
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
 * S3CustomizeFileSystem --
 *     Return a customized file system to access the s3 storage source objects.
 */
static int
S3CustomizeFileSystem(WT_STORAGE_SOURCE *storageSource, WT_SESSION *session, const char *bucketName,
  const char *authToken, const char *config, WT_FILE_SYSTEM **fileSystem)
{
    S3_FILE_SYSTEM *fs;
    S3_STORAGE *s3;
    int ret;
    std::string cacheDir, homeDir;

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
        return 1;
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

    /* Store a copy of the home directory in the file system. */
    homeDir = session->connection->get_home(session->connection);

    if ((ret = S3GetDirectory(homeDir, cacheStr, true, cacheDir)) != 0)
        return (ret);

    /* Create the file system. */
    if ((fs = (S3_FILE_SYSTEM *)calloc(1, sizeof(S3_FILE_SYSTEM))) == NULL)
        return (errno);
    fs->storage = s3;
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

    /* Add to the list of the active file systems. */
    {
        std::lock_guard<std::mutex> lockGuard(s3->fsListMutex);
        s3->fsList.push_back(fs);
    }

    *fileSystem = &fs->fileSystem;
    return (0);
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

    /* Remove from the active filesystems list. */
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
    if (ret = fs->connection->ListObjects(completePrefix, objects) != 0)
        return (ret);
    *count = objects.size();

    S3ObjectListAdd(fs->storage, objectList, objects, *count);

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
    if (ret = fs->connection->ListObjects(completePrefix, objects, 1, true) != 0)
        return (ret);
    *count = objects.size();

    S3ObjectListAdd(fs->storage, objectList, objects, *count);

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
     * Terminate any active filesystems. There are no references to the storage source, so it is
     * safe to walk the active filesystem list without a lock. The removal from the list happens
     * under a lock. Also, removal happens from the front and addition at the end, so we are safe.
     */
    while (!s3->fsList.empty()) {
        S3_FILE_SYSTEM *fs = s3->fsList.front();
        S3FileSystemTerminate(&fs->fileSystem, session);
    }

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
    return (fs->connection->PutObject(object, source));
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

    /* Linking file with the local file. */
    if (ret == 0)
        ret = chmod(destPath.c_str(), 0444);
    return ret;
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
