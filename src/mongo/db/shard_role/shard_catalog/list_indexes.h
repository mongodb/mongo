/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
enum class MONGO_MOD_NEEDS_REPLACEMENT ListIndexesInclude { kNothing, kBuildUUID, kIndexBuildInfo };

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
MONGO_MOD_PRIVATE std::vector<BSONObj> listIndexesInLock(
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
MONGO_MOD_NEEDS_REPLACEMENT std::vector<BSONObj> listIndexesEmptyListIfMissing(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nss,
    ListIndexesInclude additionalInclude,
    bool isRawDataRequest);

}  // namespace mongo
