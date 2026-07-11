// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <list>

namespace mongo {

/**
 * Corresponds to flags passed to listIndexes which specify additional information to be returned
 * for each index. BuildUUID = includeBuildUUIDs flag IndexBuildInfo = includeIndexBuildInfo flag
 */
enum class [[MONGO_MOD_NEEDS_REPLACEMENT]] ListIndexesInclude {
    kNothing,
    kBuildUUID,
    kIndexBuildInfo
};

/**
 * Returns the list of indexes for the given collection.
 *
 * Returned index specifications are not normalized: the 'collation' field is always included, even
 * when its value is simple. In contrast, in normalized index specifications, the simple collation
 * is typically omitted. For cloning purposes, these index specs can be used directly with a
 * createIndexes-style command, since normalization will occur automatically on the server. However,
 * they should not be used to create a new index if the normalization step is bypassed.
 *
 * If 'isRawDataRequest' is true, indexes on timeseries collections are returned as raw bucket
 * indexes (i.e. using internal bucket field names such as {control.min.x: 1, control.max.x: 1})
 * rather than being translated to their user-visible timeseries form (e.g. {x: 1}).
 */
[[MONGO_MOD_PRIVATE]] std::vector<BSONObj> listIndexesInLock(
    OperationContext* opCtx,
    const CollectionAcquisition& collectionAcquisition,
    ListIndexesInclude additionalInclude,
    bool isRawDataRequest);


/**
 * Retrieves all index specifications for the specified collection.
 *
 * If the collection does not exist, returns an empty list.
 *
 * Returned index specifications are not normalized: the 'collation' field is always included, even
 * when its value is simple. In contrast, in normalized index specifications, the simple collation
 * is typically omitted. For cloning purposes, these index specs can be used directly with a
 * createIndexes-style command, since normalization will occur automatically on the server. However,
 * they should not be used to create a new index if the normalization step is bypassed.
 *
 * If 'isRawDataRequest' is true, indexes on timeseries collections are returned as raw bucket
 * indexes (i.e. using internal bucket field names such as {control.min.x: 1, control.max.x: 1})
 * rather than being translated to their user-visible timeseries form (e.g. {x: 1}).
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::vector<BSONObj> listIndexesEmptyListIfMissing(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nss,
    ListIndexesInclude additionalInclude,
    bool isRawDataRequest);

}  // namespace mongo
