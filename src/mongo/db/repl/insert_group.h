// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/util/modules.h"

#include <functional>
#include <vector>

namespace mongo {
namespace repl {

/**
 * Groups consecutive insert operations on the same namespace and applies the combined operation
 * as a single oplog entry.
 * Advances the the std::vector<ApplierOperation> iterator if the grouped insert is applied
 * successfully.
 */
class InsertGroup {
    InsertGroup(const InsertGroup&) = delete;
    InsertGroup& operator=(const InsertGroup&) = delete;

public:
    using ConstIterator = std::vector<ApplierOperation>::const_iterator;
    using Mode = OplogApplication::Mode;
    typedef std::function<Status(
        OperationContext*, const OplogEntryOrGroupedInserts&, OplogApplication::Mode, bool)>
        ApplyFunc;

    InsertGroup(std::vector<ApplierOperation>& ops,
                OperationContext* opCtx,
                Mode mode,
                bool isDataConsistent,
                ApplyFunc applyOplogEntryOrGroupedInserts);

    /**
     * Attempts to group insert operations starting at 'iter'.
     * If the grouped insert is applied successfully, returns the iterator to the last standalone
     * insert operation included in the applied grouped insert.
     */
    StatusWith<ConstIterator> groupAndApplyInserts(ConstIterator oplogEntriesIterator) noexcept;

private:
    // _nextOpToGroup is used to prevent retrying bad group inserts by marking the next op to
    // attempt group inserts and not allowing further group inserts until all previous ops have been
    // processed.
    ConstIterator _nextOpToGroup;

    // Used for constructing search bounds when grouping inserts.
    ConstIterator _end;

    // Passed to _applyOplogEntryOrGroupedInserts when applying grouped inserts.
    OperationContext* _opCtx;
    Mode _mode;
    bool _isDataConsistent;

    // The function that does the actual oplog application.
    ApplyFunc _applyOplogEntryOrGroupedInserts;
};

}  // namespace repl
}  // namespace mongo
