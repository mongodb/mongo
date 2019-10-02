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
#include "mongo/db/repl/oplog_entry.h"

namespace mongo {
namespace repl {
/**
 * This is a class for a single oplog entry or grouped inserts to be applied in
 * applyOplogEntryBatch. This class is immutable and can only be initialized using either a single
 * oplog entry or a range of grouped inserts.
 */
class OplogEntryBatch {
public:
    using OperationPtrs = std::vector<const OplogEntry*>;
    using ConstIterator = OperationPtrs::const_iterator;

    OplogEntryBatch() = delete;

    // This initializes it as a single oplog entry.
    OplogEntryBatch(const OplogEntry* op) : _batch({op}) {}

    // This initializes it as grouped inserts.
    OplogEntryBatch(ConstIterator begin, ConstIterator end) : _batch(begin, end) {
        // Performs sanity checks to confirm that the batch is valid.
        invariant(!_batch.empty());
        for (auto op : _batch) {
            // Every oplog entry must be an insert.
            invariant(op->getOpType() == OpTypeEnum::kInsert);
            // Every oplog entry must be in the same namespace.
            invariant(op->getNss() == _batch.front()->getNss());
        }
    }

    // Return the oplog entry to be applied or the first oplog entry of the grouped inserts.
    const OplogEntry& getOp() const {
        return *(_batch.front());
    }

    bool isGroupedInserts() const {
        return _batch.size() > 1;
    }

    const OperationPtrs& getGroupedInserts() const {
        invariant(isGroupedInserts());
        return _batch;
    }

    // Returns a BSONObj for message logging purpose.
    BSONObj toBSON() const;

private:
    // A single oplog entry or a batch of grouped insert oplog entries to be applied.
    OperationPtrs _batch;
};
}  // namespace repl
}  // namespace mongo
