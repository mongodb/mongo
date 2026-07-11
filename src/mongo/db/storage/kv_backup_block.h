// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/filesystem.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/*
 * Represents the file blocks returned by the KVEngine during both full and incremental
 * backups. In the case of a full backup, each block is an entire file with offset=0 and
 * length=fileSize. In the case of the first basis for future incremental backups, each block is
 * an entire file with offset=0 and length=0. In the case of a subsequent incremental backup,
 * each block reflects changes made to data files since the basis (named 'thisBackupName') and
 * each block has a maximum size of 'blockSizeMB'.
 *
 * If a file is unchanged in a subsequent incremental backup, a single block is returned with
 * offset=0 and length=0. This allows consumers of the backup API to safely truncate files that
 * are not returned by the backup cursor.
 */
class KVBackupBlock final {
public:
    explicit KVBackupBlock(std::string ident,
                           std::string filePath,
                           std::uint64_t offset = 0,
                           std::uint64_t length = 0,
                           std::uint64_t fileSize = 0)
        : _ident(std::move(ident)),
          _filePath(std::move(filePath)),
          _offset(offset),
          _length(length),
          _fileSize(fileSize) {
        fassert(6355400, _filePath.has_root_directory());
    }

    ~KVBackupBlock() = default;

    std::string ident() const {
        return _ident;
    }

    std::string filePath() const {
        return _filePath.string();
    }

    std::string fileName() const {
        return _filePath.filename().string();
    }

    std::uint64_t offset() const {
        return _offset;
    }

    std::uint64_t length() const {
        return _length;
    }

    std::uint64_t fileSize() const {
        return _fileSize;
    }

private:
    const std::string _ident;
    const boost::filesystem::path _filePath;
    const std::uint64_t _offset;
    const std::uint64_t _length;
    const std::uint64_t _fileSize;
};

}  // namespace mongo
