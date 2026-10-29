// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

#include <string_view>

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

StorageEngineLockFile::StorageEngineLockFile(std::string_view dbpath, std::string_view fileName)
    : _dbpath(dbpath),
      _filespec(lockFilePath(_dbpath, fileName)),
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

Status StorageEngineLockFile::writeString(std::string_view str) {
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
