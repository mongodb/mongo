// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <vector>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/*
 * Determines which Records should be placed in the next batch of Oplog
 * writes, such that the Batched Write Policy limits are not exceeded.
 *
 * This function operates on the notion of a series of Records, owned by the
 * caller. The 'record' reference is the iterator over a series of Records and
 * 'getNextRecord' will iterate to the next record.
 *
 * The Batched Write Policy limits the size of each ApplyOps Oplog Entry:
 *  - Max physical size of entry
 *  - Max number of inserts
 *
 * The batching behavior is disabled when canBeBatched is false. The
 * InsertStatements vector will contain one element in this case.
 *
 * side effects: increments record "iterator" one past what has been batched
 * returns: vector of InsertStatements that doesn't exceed limits for a batch of
 *  Oplog ApplyOps writes.
 */
inline void buildBatchedWritesWithPolicy(size_t batchedWriteMaxSizeBytes,
                                         size_t batchedWriteMaxNumberOfInserts,
                                         std::function<boost::optional<Record>()> getNextRecord,
                                         boost::optional<Record>& record,
                                         std::vector<InsertStatement>& stmts,
                                         bool canBeBatched = true) {
    size_t curBatchSizeBytes = 0;
    if (!canBeBatched) {
        BSONObj element = record->data.getOwned().releaseToBson();
        stmts.push_back(InsertStatement(element));
        record = getNextRecord();
        return;
    }

    for (size_t count = 0; record && count < batchedWriteMaxNumberOfInserts; ++count) {
        BSONObj element = record->data.getOwned().releaseToBson();

        // Ensure size compliance with policy before batching next element.
        size_t precomputedNextSize = curBatchSizeBytes + static_cast<size_t>(element.objsize());
        if ((precomputedNextSize <= batchedWriteMaxSizeBytes) ||
            (count == 0 && element.objsize() <= BSONObjMaxUserSize)) {
            curBatchSizeBytes += static_cast<size_t>(element.objsize());
            stmts.push_back(InsertStatement(element));
            record = getNextRecord();
        } else {
            return;
        }
    }
}

}  // namespace mongo
