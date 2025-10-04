/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/bson/util/builder.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"

#include <cstddef>
#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

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
static void buildBatchedWritesWithPolicy(size_t batchedWriteMaxSizeBytes,
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
