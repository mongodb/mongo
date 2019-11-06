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

#include "mongo/db/s/persistent_task_store.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {

class BSONObj;
class NamespaceString;
class ShardId;

namespace migrationutil {

/**
 * Creates a report document with the provided parameters:
 *
 * {
 *     source:          "shard0000"
 *     destination:     "shard0001"
 *     isDonorShard:    true or false
 *     chunk:           {"min": <MinKey>, "max": <MaxKey>}
 *     collection:      "dbName.collName"
 * }
 *
 */
BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max);

// Creates a query object that can used to find overlapping ranges in the pending range deletions
// collection.
Query overlappingRangeQuery(const ChunkRange& range, const UUID& uuid);

// Checks the pending range deletions collection to see if there are any pending ranges that
// conflict with the passed in range.
bool checkForConflictingDeletions(OperationContext* opCtx,
                                  const ChunkRange& range,
                                  const UUID& uuid);

}  // namespace migrationutil

}  // namespace mongo
