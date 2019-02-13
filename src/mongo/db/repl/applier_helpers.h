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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/sync_tail.h"

namespace mongo {
namespace repl {

/**
 * Collection of helper functions and classes for oplog application.
 */
class ApplierHelpers {
public:
    using OperationPtrs = MultiApplier::OperationPtrs;

    /**
     * Sorts the oplog entries by namespace, so that entries from the same namespace will be next to
     * each other in the list.
     */
    static void stableSortByNamespace(OperationPtrs* oplogEntryPointers);

    class InsertGroup;
};

/**
 * Groups consecutive insert operations on the same namespace and applies the combined operation
 * as a single oplog entry.
 * Advances the the MultiApplier::OperationPtrs iterator if the grouped insert is applied
 * successfully.
 */
class ApplierHelpers::InsertGroup {
    MONGO_DISALLOW_COPYING(InsertGroup);

public:
    using ConstIterator = OperationPtrs::const_iterator;
    using Mode = OplogApplication::Mode;

    InsertGroup(OperationPtrs* ops, OperationContext* opCtx, Mode mode);

    /**
     * Attempts to group insert operations starting at 'iter'.
     * If the grouped insert is applied successfully, returns the iterator to the last standalone
     * insert operation included in the applied grouped insert.
     */
    StatusWith<ConstIterator> groupAndApplyInserts(ConstIterator oplogEntriesIterator);

private:
    // _doNotGroupBeforePoint is used to prevent retrying bad group inserts by marking the final op
    // of a failed group and not allowing further group inserts until that op has been processed.
    ConstIterator _doNotGroupBeforePoint;

    // Used for constructing search bounds when grouping inserts.
    ConstIterator _end;

    // Passed to _syncApply when applying grouped inserts.
    OperationContext* _opCtx;
    Mode _mode;
};

}  // namespace repl
}  // namespace mongo
