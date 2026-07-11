// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/filesystem.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

// This takes the dbpath as an input because storageGlobalParams.dbpath isn't always safe
// to access; it is up to the caller to ensure that the correct path is passed and it is
// safe to access.
int64_t getAvailableDiskSpaceBytesInDbPath(const boost::filesystem::path& dbpath);

}  // namespace mongo
