// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

struct [[MONGO_MOD_PUBLIC]] ApplierOperation {
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
class [[MONGO_MOD_PUBLIC]] OplogEntryOrGroupedInserts {
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
