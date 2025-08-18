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
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

enum class ApplicationInstruction {
    // Apply a normal oplog entry.
    applyOplogEntry,
    // Apply a prepared txn that is split from a top-level txn.
    applySplitPreparedTxnOp,
    // Apply a top-level prepared txn that can have multiple splits.
    applyTopLevelPreparedTxnOp,
};

struct MONGO_MOD_PUB ApplierOperation {
    const OplogEntry* op;
    ApplicationInstruction instruction;
    boost::optional<const InternalSessionPool::Session&> subSession;
    boost::optional<std::vector<const OplogEntry*>> preparedTxnOps;

    ApplierOperation(const OplogEntry* op,
                     ApplicationInstruction instruction = ApplicationInstruction::applyOplogEntry,
                     boost::optional<const InternalSessionPool::Session&> subSession = boost::none,
                     boost::optional<std::vector<const OplogEntry*>> preparedTxnOps = boost::none)
        : op(op),
          instruction(instruction),
          subSession(subSession),
          preparedTxnOps(preparedTxnOps) {}

    ApplierOperation(const OplogEntry* op,
                     ApplicationInstruction instruction,
                     const std::vector<OplogEntry>& preparedTxnOps)
        : op(op), instruction(instruction) {

        this->preparedTxnOps.emplace();
        this->preparedTxnOps->reserve(preparedTxnOps.size());
        std::transform(preparedTxnOps.begin(),
                       preparedTxnOps.end(),
                       std::back_inserter(*this->preparedTxnOps),
                       [](const auto& op) -> const OplogEntry* { return &op; });
    }

    const OplogEntry* operator->() const {
        return op;
    }

    const OplogEntry& operator*() const {
        return *op;
    }
};

/**
 * This is a class for a single oplog entry or grouped inserts to be applied in
 * applyOplogEntryOrGroupedInserts. This class is immutable and can only be initialized using
 * either a single oplog entry or a range of grouped inserts.
 */
class MONGO_MOD_OPEN OplogEntryOrGroupedInserts {
public:
    using ConstIterator = std::vector<ApplierOperation>::const_iterator;

    OplogEntryOrGroupedInserts() = delete;

    // This initializes it as a single oplog entry.
    OplogEntryOrGroupedInserts(ApplierOperation op) : _entryOrGroupedInserts({std::move(op)}) {}

    // This initializes it as grouped inserts.
    OplogEntryOrGroupedInserts(ConstIterator begin, ConstIterator end)
        : _entryOrGroupedInserts(begin, end) {
        // Performs sanity checks to confirm that the batch is valid.
        invariant(!_entryOrGroupedInserts.empty());
        for (const auto& op : _entryOrGroupedInserts) {
            // Every oplog entry must be an insert.
            invariant(op->getOpType() == OpTypeEnum::kInsert);
            // Every oplog entry must be in the same namespace.
            invariant(op->getNss() == _entryOrGroupedInserts.front()->getNss());
        }
    }

    // Return the oplog entry to be applied or the first oplog entry of the grouped inserts.
    const ApplierOperation& getOp() const {
        return _entryOrGroupedInserts.front();
    }

    bool isGroupedInserts() const {
        return _entryOrGroupedInserts.size() > 1;
    }

    const std::vector<ApplierOperation>& getGroupedInserts() const {
        invariant(isGroupedInserts());
        return _entryOrGroupedInserts;
    }

    // Returns a BSONObj for message logging purpose.
    BSONObj toBSON() const;

private:
    // A single oplog entry or a batch of grouped insert oplog entries to be applied.
    std::vector<ApplierOperation> _entryOrGroupedInserts;
};
}  // namespace repl
}  // namespace mongo
