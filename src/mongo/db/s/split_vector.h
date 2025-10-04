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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"

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
