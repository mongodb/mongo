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


#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cerrno>
#include <exception>
#include <memory>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

void flushMyDirectory(const boost::filesystem::path& file) {
#ifdef __linux__  // this isn't needed elsewhere
    static bool _warnedAboutFilesystem = false;
    // if called without a fully qualified path it asserts; that makes mongoperf fail.
    // so make a warning. need a better solution longer term.
    // massert(40389, str::stream() << "Couldn't find parent dir for file: " << file.string(),);
    if (!file.has_branch_path()) {
        LOGV2(22274,
              "warning flushMyDirectory couldn't find parent dir for file: {file}",
              "flushMyDirectory couldn't find parent dir for file",
              "file"_attr = file.generic_string());
        return;
    }


    boost::filesystem::path dir = file.branch_path();  // parent_path in new boosts

    LOGV2_DEBUG(22275, 1, "flushing directory {dir_string}", "dir_string"_attr = dir.string());

    int fd = ::open(dir.string().c_str(), O_RDONLY);  // DO NOT THROW OR ASSERT BEFORE CLOSING
    if (fd < 0) {
        auto ec = lastPosixError();
        msgasserted(40387,
                    str::stream() << "Couldn't open directory '" << dir.string()
                                  << "' for flushing: " << errorMessage(ec));
    }
    if (fsync(fd) != 0) {
        auto ec = lastPosixError();
        if (ec == posixError(EINVAL)) {  // indicates filesystem does not support synchronization
            if (!_warnedAboutFilesystem) {
                LOGV2_OPTIONS(
                    22276,
                    {logv2::LogTag::kStartupWarnings},
                    "This file system is not supported. For further information see: "
                    "http://dochub.mongodb.org/core/unsupported-filesystems Please notify MongoDB, "
                    "Inc. if an unlisted filesystem generated this warning");
                _warnedAboutFilesystem = true;
            }
        } else {
            close(fd);
            massert(40388,
                    str::stream() << "Couldn't fsync directory '" << dir.string()
                                  << "': " << errorMessage(ec),
                    false);
        }
    }
    close(fd);
#endif
}
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

StorageEngineLockFile::StorageEngineLockFile(const std::string& dbpath, StringData fileName)
    : _dbpath(dbpath),
      _filespec((boost::filesystem::path(_dbpath) / fileName.toString()).string()),
      _uncleanShutdown(boost::filesystem::exists(_filespec) &&
                       boost::filesystem::file_size(_filespec) > 0),
      _lockFileHandle(new LockFileHandle()) {}

StorageEngineLockFile::~StorageEngineLockFile() {
    close();
}

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

    // Use file permissions 644
    int lockFile =
        ::open(_filespec.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (lockFile < 0) {
        auto ec = lastPosixError();
        if (ec == posixError(EACCES)) {
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
    int ret = ::flock(lockFile, LOCK_EX | LOCK_NB);
    if (ret != 0) {
        auto ec = lastPosixError();
        ::close(lockFile);
        return Status(ErrorCodes::DBPathInUse,
                      str::stream() << "Unable to lock the lock file: " << _filespec << " ("
                                    << errorMessage(ec) << ")."
                                    << " Another mongod instance is already running on the "
                                    << _dbpath << " directory");
    }
    _lockFileHandle->_fd = lockFile;
    return Status::OK();
}

void StorageEngineLockFile::close() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    ::flock(_lockFileHandle->_fd, LOCK_UN);
    ::close(_lockFileHandle->_fd);
    _lockFileHandle->clear();
}

Status StorageEngineLockFile::writeString(StringData str) {
    if (!_lockFileHandle->isValid()) {
        return Status(ErrorCodes::FileNotOpen,
                      str::stream() << "Unable to write string to " << _filespec
                                    << " because file has not been opened.");
    }

    if (::ftruncate(_lockFileHandle->_fd, 0)) {
        auto ec = lastPosixError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write string to file (ftruncate failed): "
                                    << _filespec << ' ' << errorMessage(ec));
    }

    int bytesWritten = ::write(_lockFileHandle->_fd, str.rawData(), str.size());
    if (bytesWritten < 0) {
        auto ec = lastPosixError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write string " << str << " to file: " << _filespec
                                    << ' ' << errorMessage(ec));

    } else if (bytesWritten == 0) {
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unable to write string " << str << " to file: " << _filespec
                                    << " no data written.");
    }

    if (::fsync(_lockFileHandle->_fd)) {
        auto ec = lastPosixError();
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream()
                          << "Unable to write process id " << str
                          << " to file (fsync failed): " << _filespec << ' ' << errorMessage(ec));
    }

    flushMyDirectory(_filespec);

    return Status::OK();
}

void StorageEngineLockFile::clearPidAndUnlock() {
    if (!_lockFileHandle->isValid()) {
        return;
    }
    LOGV2(22279, "shutdown: removing fs lock...");
    // This ought to be an unlink(), but Eliot says the last
    // time that was attempted, there was a race condition
    // with StorageEngineLockFile::open().
    if (::ftruncate(_lockFileHandle->_fd, 0)) {
        auto ec = lastPosixError();
        LOGV2(22280, "Couldn't remove fs lock", "error"_attr = errorMessage(ec));
    }
    close();
}

}  // namespace mongo
