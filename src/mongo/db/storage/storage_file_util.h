// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Perform an fsync on the file.
 */
Status fsyncFile(const boost::filesystem::path& path);

/**
 * Perform an fsync on the parent directory of 'file'.
 */
Status fsyncParentDirectory(const boost::filesystem::path& file);

/**
 * Perform a filesystem rename from 'source' to 'dest'. Performs an fsync on the destination file
 * and the parent directories of both 'source' and 'dest'.
 *
 * Returns a FileRenameFailed error if the destination file already exists.
 */
Status fsyncRename(const boost::filesystem::path& source, const boost::filesystem::path& dest);

}  // namespace mongo
