// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
