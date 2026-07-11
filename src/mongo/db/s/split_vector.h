// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;

class OperationContext;

/**
 * Given a chunk, determines whether it can be split and returns the split points if so. This
 * function is functionally equivalent to the splitVector command.
 *
 * If maxSplitPoints is specified and there are more than "maxSplitPoints" split points,
 * only the first "maxSplitPoints" points are returned.
 * If maxChunkObjects is specified then it indicates to split every "maxChunkObjects"th key.
 * By default, we split so that each new chunk has approximately half the keys of the maxChunkSize
 * chunk. We only split at the "maxChunkObjects"th key if it would split at a lower key count than
 * the default.
 * maxChunkSize is the maximum size of a chunk in megabytes. If the chunk exceeds this size, we
 * should split. Although maxChunkSize and maxChunkSizeBytes are boost::optional, at least one must
 * be specified.
 * If force is set, split at the halfway point of the chunk. This also effectively
 * makes maxChunkSize equal the size of the chunk.
 */
std::vector<BSONObj> splitVector(OperationContext* opCtx,
                                 const CollectionAcquisition& collection,
                                 const BSONObj& keyPattern,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 bool force,
                                 boost::optional<long long> maxSplitPoints,
                                 boost::optional<long long> maxChunkObjects,
                                 boost::optional<long long> maxChunkSizeBytes);

}  // namespace mongo
