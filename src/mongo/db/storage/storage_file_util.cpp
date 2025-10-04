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


#include "mongo/db/storage/storage_file_util.h"

#include <cerrno>
#include <string>
#include <system_error>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#ifdef __linux__
#include <fcntl.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/file.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


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

    LOGV2_DEBUG(22289, 1, "flushing directory {dir_string}", "dir_string"_attr = dir.string());

    int fd = ::open(dir.string().c_str(), O_RDONLY);
    if (fd < 0) {
        auto ec = lastPosixError();
        return {ErrorCodes::FileOpenFailed,
                str::stream() << "Failed to open directory " << dir.string()
                              << " for flushing: " << errorMessage(ec)};
    }
    if (fsync(fd) != 0) {
        auto ec = lastPosixError();
        if (ec == posixError(EINVAL)) {
            LOGV2_WARNING(22290,
                          "Could not fsync directory because this file system is not supported.");
        } else {
            close(fd);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to fsync directory '" << dir.string()
                                  << "': " << errorMessage(ec)};
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
