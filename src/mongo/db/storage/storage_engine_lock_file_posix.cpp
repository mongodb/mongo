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
#include <fcntl.h>
#include <ostream>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mongo/db/storage/paths.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

const std::string kLockFileBasename = "mongod.lock";

}  // namespace

class StorageEngineLockFile::LockFileHandle {
public:
    static const int kInvalidFd = -1;
    LockFileHandle() : _fd(kInvalidFd) {}
    bool isValid() const {
        return _fd != kInvalidFd;
    }
    void clear() {
        _fd = kInvalidFd;
    }
    int _fd;
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

    // Use file permissions 644
    int lockFile =
        ::open(_filespec.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (lockFile < 0) {
        int errorcode = errno;
        if (errorcode == EACCES) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream()
                              << "Attempted to create a lock file on a read-only directory: "
                              << _dbpath);
        }
        return Status(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to create/open lock file: " << _filespec << ' '
                                    << errnoWithDescription(errorcode)
                                    << " Is a mongod instance already running?");
    }
#if !defined(__sun)
    int ret = ::flock(lockFile, LOCK_EX | LOCK_NB);
#else
    struct flock fileLockInfo = {0};
    fileLockInfo.l_type = F_WRLCK;
    fileLockInfo.l_whence = SEEK_SET;
    int ret = ::fcntl(lockFile, F_SETLK, &fileLockInfo);
#endif  // !defined(__sun)
    if (ret != 0) {
        int errorcode = errno;
        ::close(lockFile);
        return Status(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to lock file: " << _filespec << ' '
                                    << errnoWithDescription(errorcode)
                                    << ". Is a mongod instance already running?");
    }
    _lockFileHandle->_fd = lockFile;
    return Status::OK();
}

void StorageEngineLockFile::close() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    ::close(_lockFileHandle->_fd);
    _lockFileHandle->clear();
}

Status StorageEngineLockFile::writePid() {
    if (!_lockFileHandle->isValid()) {
        return Status(ErrorCodes::FileNotOpen,
                      str::stream() << "Unable to write process ID to " << _filespec
                                    << " because file has not been opened.");
    }

    if (::ftruncate(_lockFileHandle->_fd, 0)) {
        int errorcode = errno;
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write process id to file (ftruncate failed): "
                                    << _filespec
                                    << ' '
                                    << errnoWithDescription(errorcode));
    }

    ProcessId pid = ProcessId::getCurrent();
    std::stringstream ss;
    ss << pid << std::endl;
    std::string pidStr = ss.str();
    int bytesWritten = ::write(_lockFileHandle->_fd, pidStr.c_str(), pidStr.size());
    if (bytesWritten < 0) {
        int errorcode = errno;
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

    if (::fsync(_lockFileHandle->_fd)) {
        int errorcode = errno;
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write process id " << pid.toString()
                                    << " to file (fsync failed): "
                                    << _filespec
                                    << ' '
                                    << errnoWithDescription(errorcode));
    }

    flushMyDirectory(_filespec);

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
    if (::ftruncate(_lockFileHandle->_fd, 0)) {
        int errorcode = errno;
        log() << "couldn't remove fs lock " << errnoWithDescription(errorcode);
    }
#if !defined(__sun)
    ::flock(_lockFileHandle->_fd, LOCK_UN);
#else
    struct flock fileLockInfo = {0};
    fileLockInfo.l_type = F_UNLCK;
    fileLockInfo.l_whence = SEEK_SET;
    ::fcntl(_lockFileHandle->_fd, F_SETLK, &fileLockInfo);
#endif  // !defined(__sun)
}

}  // namespace mongo
