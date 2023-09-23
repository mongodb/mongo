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
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <fstream>  // IWYU pragma: keep
#include <system_error>
#include <vector>

#ifdef __linux__  // Only needed by flushDirectory for Linux
#include <boost/filesystem/path.hpp>
#include <fcntl.h>
#endif

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/file.h"
#include "mongo/util/str.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace dps = ::mongo::dotted_path_support;

namespace {

const std::string kMetadataBasename = "storage.bson";

}  // namespace

bool fsyncFile(boost::filesystem::path path) {
    invariant(path.has_filename());
    File file;
    file.open(path.string().c_str(), /*read-only*/ false, /*direct-io*/ false);
    if (!file.is_open()) {
        return false;
    }
    file.fsync();
    return true;
}

// static
std::unique_ptr<StorageEngineMetadata> StorageEngineMetadata::forPath(const std::string& dbpath) {
    std::unique_ptr<StorageEngineMetadata> metadata;
    if (boost::filesystem::exists(boost::filesystem::path(dbpath) / kMetadataBasename)) {
        metadata.reset(new StorageEngineMetadata(dbpath));
        Status status = metadata->read();
        if (!status.isOK()) {
            LOGV2_FATAL_NOTRACE(28661,
                                "Unable to read the storage engine metadata file: {error}",
                                "Unable to read the storage engine metadata file",
                                "error"_attr = status);
        }
    }
    return metadata;
}

// static
boost::optional<std::string> StorageEngineMetadata::getStorageEngineForPath(
    const std::string& dbpath) {
    if (auto metadata = StorageEngineMetadata::forPath(dbpath)) {
        return {metadata->getStorageEngine()};
    }

    return {};
}

StorageEngineMetadata::StorageEngineMetadata(const std::string& dbpath) : _dbpath(dbpath) {
    reset();
}

StorageEngineMetadata::~StorageEngineMetadata() {}

void StorageEngineMetadata::reset() {
    _storageEngine.clear();
    _storageEngineOptions = BSONObj();
}

const std::string& StorageEngineMetadata::getStorageEngine() const {
    return _storageEngine;
}

const BSONObj& StorageEngineMetadata::getStorageEngineOptions() const {
    return _storageEngineOptions;
}

void StorageEngineMetadata::setStorageEngine(const std::string& storageEngine) {
    _storageEngine = storageEngine;
}

void StorageEngineMetadata::setStorageEngineOptions(const BSONObj& storageEngineOptions) {
    _storageEngineOptions = storageEngineOptions.getOwned();
}

Status StorageEngineMetadata::read() {
    reset();

    boost::filesystem::path metadataPath = boost::filesystem::path(_dbpath) / kMetadataBasename;

    if (!boost::filesystem::exists(metadataPath)) {
        return Status(ErrorCodes::NonExistentPath,
                      str::stream() << "Metadata file " << metadataPath.string() << " not found.");
    }

    boost::uintmax_t fileSize = boost::filesystem::file_size(metadataPath);
    if (fileSize == 0) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream()
                          << "Metadata file " << metadataPath.string() << " cannot be empty.");
    }
    if (fileSize == static_cast<boost::uintmax_t>(-1)) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream()
                          << "Unable to determine size of metadata file " << metadataPath.string());
    }

    std::vector<char> buffer(fileSize);
    try {
        std::ifstream ifs(metadataPath.c_str(), std::ios_base::in | std::ios_base::binary);
        if (!ifs) {
            return Status(ErrorCodes::FileNotOpen,
                          str::stream()
                              << "Failed to read metadata from " << metadataPath.string());
        }

        // Read BSON from file
        ifs.read(&buffer[0], buffer.size());
        if (!ifs) {
            return Status(ErrorCodes::FileStreamFailed,
                          str::stream()
                              << "Unable to read BSON data from " << metadataPath.string());
        }
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::FileStreamFailed,
                      str::stream() << "Unexpected error reading BSON data from "
                                    << metadataPath.string() << ": " << ex.what());
    }

    ConstDataRange cdr(&buffer[0], buffer.size());
    auto swObj = cdr.readNoThrow<Validated<BSONObj>>();
    if (!swObj.isOK()) {
        return swObj.getStatus();
    }

    BSONObj obj = swObj.getValue();

    // Validate 'storage.engine' field.
    BSONElement storageEngineElement = dps::extractElementAtPath(obj, "storage.engine");
    if (storageEngineElement.type() != mongo::String) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "The 'storage.engine' field in metadata must be a string: "
                                    << storageEngineElement.toString());
    }

    // Extract storage engine name from 'storage.engine' node.
    std::string storageEngine = storageEngineElement.String();
    if (storageEngine.empty()) {
        return Status(ErrorCodes::FailedToParse,
                      "The 'storage.engine' field in metadata cannot be empty string.");
    }
    _storageEngine = storageEngine;

    // Read storage engine options generated by storage engine factory from startup options.
    BSONElement storageEngineOptionsElement = dps::extractElementAtPath(obj, "storage.options");
    if (!storageEngineOptionsElement.eoo()) {
        if (!storageEngineOptionsElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "The 'storage.options' field in metadata must be an object: "
                              << storageEngineOptionsElement.toString());
        }
        setStorageEngineOptions(storageEngineOptionsElement.Obj());
    }

    return Status::OK();
}

void flushMyDirectory(const boost::filesystem::path& file) {
#ifdef __linux__  // this isn't needed elsewhere
    static bool _warnedAboutFilesystem = false;
    // if called without a fully qualified path it asserts; that makes mongoperf fail.
    // so make a warning. need a better solution longer term.
    // massert(13652, str::stream() << "Couldn't find parent dir for file: " << file.string(),);
    if (!file.has_branch_path()) {
        LOGV2(22283,
              "warning flushMyDirectory couldn't find parent dir for file: {file}",
              "flushMyDirectory couldn't find parent dir for file",
              "file"_attr = file.generic_string());
        return;
    }


    boost::filesystem::path dir = file.branch_path();  // parent_path in new boosts

    LOGV2_DEBUG(22284, 1, "flushing directory {dir_string}", "dir_string"_attr = dir.string());

    int fd = ::open(dir.string().c_str(), O_RDONLY);  // DO NOT THROW OR ASSERT BEFORE CLOSING
    if (fd < 0) {
        auto ec = lastPosixError();
        msgasserted(13650,
                    str::stream() << "Couldn't open directory '" << dir.string()
                                  << "' for flushing: " << errorMessage(ec));
    }
    if (fsync(fd) != 0) {
        auto ec = lastPosixError();
        if (ec == posixError(EINVAL)) {  // indicates filesystem does not support synchronization
            if (!_warnedAboutFilesystem) {
                LOGV2_OPTIONS(
                    22285,
                    {logv2::LogTag::kStartupWarnings},
                    "This file system is not supported. For further information see: "
                    "http://dochub.mongodb.org/core/unsupported-filesystems Please notify MongoDB, "
                    "Inc. if an unlisted filesystem generated this warning");
                _warnedAboutFilesystem = true;
            }
        } else {
            close(fd);
            msgasserted(13651,
                        str::stream() << "Couldn't fsync directory '" << dir.string()
                                      << "': " << errorMessage(ec));
        }
    }
    close(fd);
#endif
}

Status StorageEngineMetadata::write() const {
    if (_storageEngine.empty()) {
        return Status(ErrorCodes::BadValue,
                      "Cannot write empty storage engine name to metadata file.");
    }

    boost::filesystem::path metadataTempPath =
        boost::filesystem::path(_dbpath) / (kMetadataBasename + ".tmp");
    {
        std::ofstream ofs(metadataTempPath.c_str(), std::ios_base::out | std::ios_base::binary);
        if (!ofs) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::FileNotOpen,
                          str::stream() << "Failed to write metadata to "
                                        << metadataTempPath.string() << ": " << errorMessage(ec));
        }

        BSONObj obj = BSON(
            "storage" << BSON("engine" << _storageEngine << "options" << _storageEngineOptions));
        ofs.write(obj.objdata(), obj.objsize());
        if (!ofs) {
            auto ec = lastSystemError();
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to write BSON data to "
                                        << metadataTempPath.string() << ": " << errorMessage(ec));
        }
    }

    // Rename temporary file to actual metadata file.
    boost::filesystem::path metadataPath = boost::filesystem::path(_dbpath) / kMetadataBasename;
    try {
        // Renaming a file (at least on POSIX) should:
        // 1) fsync the temporary file.
        // 2) perform the rename.
        // 3) fsync the to and from directory (in this case, both to and from are the same).
        if (!fsyncFile(metadataTempPath)) {
            return Status(ErrorCodes::FileRenameFailed,
                          str::stream() << "Failed to fsync new `storage.bson` file.");
        }
        boost::filesystem::rename(metadataTempPath, metadataPath);
        flushMyDirectory(metadataPath);
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::FileRenameFailed,
                      str::stream() << "Unexpected error while renaming temporary metadata file "
                                    << metadataTempPath.string() << " to " << metadataPath.string()
                                    << ": " << ex.what());
    }

    return Status::OK();
}

template <>
Status StorageEngineMetadata::validateStorageEngineOption<bool>(
    StringData fieldName, bool expectedValue, boost::optional<bool> defaultValue) const {
    BSONElement element = _storageEngineOptions.getField(fieldName);
    if (element.eoo()) {
        if (defaultValue && *defaultValue != expectedValue) {
            return Status(
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "Requested option conflicts with the current storage engine option for "
                    << fieldName << "; you requested " << (expectedValue ? "true" : "false")
                    << " but the current server storage is implicitly set to "
                    << (*defaultValue ? "true" : "false") << " and cannot be changed");
        }
        return Status::OK();
    }
    if (!element.isBoolean()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Expected boolean field " << fieldName << " but got "
                                    << typeName(element.type()) << " instead: " << element);
    }
    if (element.boolean() == expectedValue) {
        return Status::OK();
    }
    return Status(
        ErrorCodes::InvalidOptions,
        str::stream() << "Requested option conflicts with current storage engine option for "
                      << fieldName << "; you requested " << (expectedValue ? "true" : "false")
                      << " but the current server storage is already set to "
                      << (element.boolean() ? "true" : "false") << " and cannot be changed");
}

}  // namespace mongo
