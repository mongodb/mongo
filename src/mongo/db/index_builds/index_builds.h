// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <tuple>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Describes an index build on a collection.
 */
struct IndexBuildsEntry {
    DatabaseName dbName;

    // Collection UUID.
    const UUID collUUID;

    // Index specs for the build.
    std::vector<std::tuple<BSONObj, std::string>> indexSpecsAndIdents;
};

/**
 * IndexBuilds is a mapping from index build UUID to details about how to start the index build.
 */
using IndexBuilds = stdx::unordered_map<UUID, IndexBuildsEntry, UUID::Hash>;

}  // namespace mongo
