/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_lock_file.h"

#include <boost/filesystem.hpp>
#include <io.h>
#include <ostream>
#include <sstream>

#include "mongo/platform/process_id.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

const std::string kLockFileBasename = "mongod.lock";

Status _truncateFile(HANDLE handle) {
    invariant(handle != INVALID_HANDLE_VALUE);

    LARGE_INTEGER largeint;
    largeint.QuadPart = 0;
    if (::SetFilePointerEx(handle, largeint, NULL, FILE_BEGIN) == FALSE) {
        int errorcode = GetLastError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to truncate lock file (SetFilePointerEx failed) "
                                    << errnoWithDescription(errorcode));
    }

    if (::SetEndOfFile(handle) == FALSE) {
        int errorcode = GetLastError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to truncate lock file (SetEndOfFile failed) "
                                    << errnoWithDescription(errorcode));
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

StorageEngineLockFile::StorageEngineLockFile(const std::string& dbpath)
    : _dbpath(dbpath),
      _filespec((boost::filesystem::path(_dbpath) / kLockFileBasename).string()),
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
            return Status(ErrorCodes::NonExistentPath,
                          str::stream() << "Data directory " << _dbpath << " not found.");
        }
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::UnknownError,
                      str::stream() << "Unable to check existence of data directory " << _dbpath
                                    << ": "
                                    << ex.what());
    }

    HANDLE lockFileHandle = CreateFileA(_filespec.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        0 /* do not allow anyone else access */,
                                        NULL,
                                        OPEN_ALWAYS /* success if fh can open */,
                                        0,
                                        NULL);

    if (lockFileHandle == INVALID_HANDLE_VALUE) {
        int errorcode = GetLastError();
        if (errorcode == ERROR_ACCESS_DENIED) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream()
                              << "Attempted to create a lock file on a read-only directory: "
                              << _dbpath);
        }
        return Status(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to create/open lock file: " << _filespec << ' '
                                    << errnoWithDescription(errorcode)
                                    << ". Is a mongod instance already running?");
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

Status StorageEngineLockFile::writePid() {
    if (!_lockFileHandle->isValid()) {
        return Status(ErrorCodes::FileNotOpen,
                      str::stream() << "Unable to write process ID to " << _filespec
                                    << " because file has not been opened.");
    }

    Status status = _truncateFile(_lockFileHandle->_handle);
    if (!status.isOK()) {
        return status;
    }

    ProcessId pid = ProcessId::getCurrent();
    std::stringstream ss;
    ss << pid << std::endl;
    std::string pidStr = ss.str();
    DWORD bytesWritten = 0;
    if (::WriteFile(_lockFileHandle->_handle,
                    static_cast<LPCVOID>(pidStr.c_str()),
                    static_cast<DWORD>(pidStr.size()),
                    &bytesWritten,
                    NULL) == FALSE) {
        int errorcode = GetLastError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write process id " << pid.toString()
                                    << " to file: "
                                    << _filespec
                                    << ' '
                                    << errnoWithDescription(errorcode));
    } else if (bytesWritten == 0) {
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write process id " << pid.toString()
                                    << " to file: "
                                    << _filespec
                                    << " no data written.");
    }

    ::FlushFileBuffers(_lockFileHandle->_handle);

    return Status::OK();
}

void StorageEngineLockFile::clearPidAndUnlock() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    log() << "shutdown: removing fs lock...";
    // This ought to be an unlink(), but Eliot says the last
    // time that was attempted, there was a race condition
    // with StorageEngineLockFile::open().
    Status status = _truncateFile(_lockFileHandle->_handle);
    if (!status.isOK()) {
        log() << "couldn't remove fs lock " << status.toString();
    }
    CloseHandle(_lockFileHandle->_handle);
    _lockFileHandle->clear();
}

}  // namespace mongo
