
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/service_context.h"

namespace mongo {

constexpr StringData kLockFileBasename = "mongod.lock"_sd;

class StorageEngineLockFile {
    MONGO_DISALLOW_COPYING(StorageEngineLockFile);

public:
    static boost::optional<StorageEngineLockFile>& get(ServiceContext* service);

    /**
     * Checks existing lock file, if present, to see if it contains data from a previous
     * unclean shutdown. A clean shutdown should have produced a zero length lock file.
     * Uses open() to read existing lock file or create new file.
     * Uses boost::filesystem to check lock file so may throw boost::exception.
     */
    StorageEngineLockFile(const std::string& dbpath, StringData fileName = kLockFileBasename);

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
     * Truncates file contents and releases file locks.
     */
    void clearPidAndUnlock();

private:
    std::string _dbpath;
    std::string _filespec;
    bool _uncleanShutdown;

    class LockFileHandle;
    std::unique_ptr<LockFileHandle> _lockFileHandle;
};

}  // namespace mongo
