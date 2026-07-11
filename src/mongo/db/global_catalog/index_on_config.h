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
struct IndexSpec_ForCatalog {
    BSONObj keys;
    bool unique;
};

/**
 * Returns the index specs that must exist on every chunks collection (both the
 * config-server's config.chunks and a shard's config.shard.catalog.chunks).
 */
[[MONGO_MOD_PUBLIC]] std::vector<IndexSpec_ForCatalog> getChunkCollectionIndexSpecs();

/**
 * Ensures that the given namespace exists and has all of the indexes described
 * by `specs`.  Creates the collection if it doesn't yet exist.  Does not error
 * if an index already exists, so long as the options are the same.
 */
[[MONGO_MOD_PUBLIC]] Status ensureCollectionIndexes(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const std::vector<IndexSpec_ForCatalog>& specs);

/**
 * Builds an index on a config server collection.
 * Creates the collection if it doesn't yet exist.  Does not error if the index already exists,
 * so long as the options are the same.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createIndexOnConfigCollection(OperationContext* opCtx,
                                                                     const NamespaceString& ns,
                                                                     const BSONObj& keys,
                                                                     bool unique);

}  // namespace mongo
