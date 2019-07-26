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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/storage_file_util.h"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <boost/filesystem/path.hpp>

#include "mongo/util/file.h"
#include "mongo/util/log.h"

namespace mongo {

Status fsyncFile(const boost::filesystem::path& path) {
    File file;
    file.open(path.string().c_str(), /*read-only*/ false, /*direct-io*/ false);
    if (!file.is_open()) {
        return {ErrorCodes::FileOpenFailed,
                str::stream() << "Failed to open file " << path.string()};
    }
    file.fsync();
    return Status::OK();
}

Status fsyncParentDirectory(const boost::filesystem::path& file) {
#ifdef __linux__  // this isn't needed elsewhere
    if (!file.has_parent_path()) {
        return {ErrorCodes::InvalidPath,
                str::stream() << "Couldn't find parent directory for file: " << file.string()};
    }

    boost::filesystem::path dir = file.parent_path();

    LOG(1) << "flushing directory " << dir.string();

    int fd = ::open(dir.string().c_str(), O_RDONLY);
    if (fd < 0) {
        return {ErrorCodes::FileOpenFailed,
                str::stream() << "Failed to open directory " << dir.string()
                              << " for flushing: " << errnoWithDescription()};
    }
    if (fsync(fd) != 0) {
        int e = errno;
        if (e == EINVAL) {
            warning() << "Could not fsync directory because this file system is not supported.";
        } else {
            close(fd);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to fsync directory '" << dir.string()
                                  << "': " << errnoWithDescription(e)};
        }
    }
    close(fd);
#endif
    return Status::OK();
}

Status fsyncRename(const boost::filesystem::path& source, const boost::filesystem::path& dest) {
    if (boost::filesystem::exists(dest)) {
        return {ErrorCodes::FileRenameFailed,
                "Attempted to rename file to an existing file: " + dest.string()};
    }

    boost::system::error_code ec;
    boost::filesystem::rename(source, dest, ec);
    if (ec) {
        return {ErrorCodes::FileRenameFailed,
                str::stream() << "Error renaming data file from " << source.string() << " to "
                              << dest.string() << ": " << ec.message()};
    }
    auto status = fsyncFile(dest);
    if (!status.isOK()) {
        return status;
    }

    status = fsyncParentDirectory(source);
    if (!status.isOK()) {
        return status;
    }

    status = fsyncParentDirectory(dest);
    if (!status.isOK()) {
        return status;
    }
    return Status::OK();
}


}  // namespace mongo
