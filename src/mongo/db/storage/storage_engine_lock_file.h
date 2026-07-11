// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

[[MONGO_MOD_FILE_PRIVATE]] constexpr std::string_view kLockFileBasename = "mongod.lock"sv;

class [[MONGO_MOD_PUBLIC]] StorageEngineLockFile {
    StorageEngineLockFile(const StorageEngineLockFile&) = delete;
    StorageEngineLockFile& operator=(const StorageEngineLockFile&) = delete;

public:
    static boost::optional<StorageEngineLockFile>& get(ServiceContext* service);

    /**
     * Returns the path where a lockfile would be expected to live given the dbpath.
     */
    static std::string lockFilePath(std::string_view dbpath,
                                    std::string_view fileName = kLockFileBasename);

    /**
     * Creates the lock file used to prevent concurrent processes from accessing the data files,
     * as appropriate.
     */
    static void create(ServiceContext* service, std::string_view dbpath);

    /**
     * Checks existing lock file, if present, to see if it contains data from a previous
     * unclean shutdown. A clean shutdown should have produced a zero length lock file.
     * Uses open() to read existing lock file or create new file.
     * Uses boost::filesystem to check lock file so may throw boost::exception.
     */
    StorageEngineLockFile(std::string_view dbpath, std::string_view fileName = kLockFileBasename);

    virtual ~StorageEngineLockFile();

    /**
     * Returns the path to the lock file.
     */
    std::string getFilespec() const;

    /**
     * Returns true if lock file was not zeroed out due to previous unclean shutdown.
     * This state is evaluated at object initialization to allow storage engine
     * to make decisions on recovery based on this information after open() has been called.
     */
    bool createdByUncleanShutdown() const;

    /**
     * Opens and locks 'mongod.lock' in 'dbpath' directory.
     */
    Status open();

    /**
     * Closes lock file handles.
     */
    void close();

    /**
     * Writes current process ID to file.
     * Fails if lock file has not been opened.
     */
    Status writePid();

    /**
     * Writes the string to file.
     * Fails if lock file has not been opened.
     */
    Status writeString(std::string_view str);

    /**
     * Truncates file contents and releases file locks.
     */
    void clearPidAndUnlock();

private:
    std::string _getNonExistentPathMessage() const;

    std::string _dbpath;
    std::string _filespec;
    bool _uncleanShutdown;

    class LockFileHandle;
    std::unique_ptr<LockFileHandle> _lockFileHandle;
};

}  // namespace mongo
