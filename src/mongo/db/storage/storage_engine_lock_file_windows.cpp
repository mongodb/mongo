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


#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

#include <io.h>

#include <boost/filesystem.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

Status _truncateFile(HANDLE handle) {
    invariant(handle != INVALID_HANDLE_VALUE);

    LARGE_INTEGER largeint;
    largeint.QuadPart = 0;
    if (::SetFilePointerEx(handle, largeint, NULL, FILE_BEGIN) == FALSE) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to truncate lock file (SetFilePointerEx failed) "
                                    << errorMessage(ec));
    }

    if (::SetEndOfFile(handle) == FALSE) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to truncate lock file (SetEndOfFile failed) "
                                    << errorMessage(ec));
    }

    return Status::OK();
}

}  // namespace

class StorageEngineLockFile::LockFileHandle {
public:
    LockFileHandle() : _handle(INVALID_HANDLE_VALUE) {}
    bool isValid() const {
        return _handle != INVALID_HANDLE_VALUE;
    }
    void clear() {
        _handle = INVALID_HANDLE_VALUE;
    }
    HANDLE _handle;
};

StorageEngineLockFile::StorageEngineLockFile(StringData dbpath, StringData fileName)
    : _dbpath(dbpath),
      _filespec((boost::filesystem::path(_dbpath) / std::string{fileName}).string()),
      _uncleanShutdown(boost::filesystem::exists(_filespec) &&
                       boost::filesystem::file_size(_filespec) > 0),
      _lockFileHandle(new LockFileHandle()) {}

StorageEngineLockFile::~StorageEngineLockFile() {}

std::string StorageEngineLockFile::getFilespec() const {
    return _filespec;
}

bool StorageEngineLockFile::createdByUncleanShutdown() const {
    return _uncleanShutdown;
}

Status StorageEngineLockFile::open() {
    try {
        if (!boost::filesystem::exists(_dbpath)) {
            return Status(ErrorCodes::NonExistentPath, _getNonExistentPathMessage());
        }
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::UnknownError,
                      str::stream() << "Unable to check existence of data directory " << _dbpath
                                    << ": " << ex.what());
    }

    HANDLE lockFileHandle = CreateFileW(toNativeString(_filespec.c_str()).c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ /* only allow readers access */,
                                        NULL,
                                        OPEN_ALWAYS /* success if fh can open */,
                                        0,
                                        NULL);

    if (lockFileHandle == INVALID_HANDLE_VALUE) {
        auto ec = lastSystemError();
        if (ec == systemError(ERROR_ACCESS_DENIED)) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream()
                              << "Attempted to create a lock file on a read-only directory: "
                              << _dbpath);
        }
        return Status(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to create/open the lock file: " << _filespec << " ("
                                    << errorMessage(ec) << ")."
                                    << " Ensure the user executing mongod is the owner of the lock "
                                       "file and has the appropriate permissions. Also make sure "
                                       "that another mongod instance is not already running on the "
                                    << _dbpath << " directory");
    }
    _lockFileHandle->_handle = lockFileHandle;
    return Status::OK();
}

void StorageEngineLockFile::close() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    CloseHandle(_lockFileHandle->_handle);
    _lockFileHandle->clear();
}

Status StorageEngineLockFile::writeString(StringData str) {
    if (!_lockFileHandle->isValid()) {
        return Status(ErrorCodes::FileNotOpen,
                      str::stream() << "Unable to write string to " << _filespec
                                    << " because file has not been opened.");
    }

    Status status = _truncateFile(_lockFileHandle->_handle);
    if (!status.isOK()) {
        return status;
    }

    DWORD bytesWritten = 0;
    if (::WriteFile(_lockFileHandle->_handle,
                    static_cast<LPCVOID>(str.data()),
                    static_cast<DWORD>(str.size()),
                    &bytesWritten,
                    NULL) == FALSE) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write string " << str << " to file: " << _filespec
                                    << ' ' << errorMessage(ec));
    } else if (bytesWritten == 0) {
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write string " << str << " to file: " << _filespec
                                    << " no data written.");
    }

    ::FlushFileBuffers(_lockFileHandle->_handle);

    return Status::OK();
}

void StorageEngineLockFile::clearPidAndUnlock() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    LOGV2(22281, "shutdown: removing fs lock...");
    // This ought to be an unlink(), but Eliot says the last
    // time that was attempted, there was a race condition
    // with StorageEngineLockFile::open().
    Status status = _truncateFile(_lockFileHandle->_handle);
    if (!status.isOK()) {
        LOGV2(22282, "Couldn't remove fs lock", "error"_attr = status);
    }
    CloseHandle(_lockFileHandle->_handle);
    _lockFileHandle->clear();
}

}  // namespace mongo
