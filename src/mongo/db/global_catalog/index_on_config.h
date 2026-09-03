// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Describes a single index to create: the key pattern and whether it is unique.
 */
struct [[MONGO_MOD_PUBLIC]] IndexSpec_ForCatalog {
    BSONObj keys;
    bool unique;
};

/**
 * Returns the index specs that must exist on every chunks collection (both the
 * config-server's config.chunks and a shard's config.shard.catalog.chunks).
 */
[[MONGO_MOD_PUBLIC]] std::vector<IndexSpec_ForCatalog> getChunkCollectionIndexSpecs();

/**
 * Returns the index specs that must exist on the config.placementHistory collection.
 */
[[MONGO_MOD_PUBLIC]] std::vector<IndexSpec_ForCatalog> getPlacementHistoryCollectionIndexSpecs();

}  // namespace mongo
