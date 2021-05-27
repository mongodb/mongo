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

#include "mongo/base/status_with.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_applier.h"

namespace mongo {
namespace repl {

/**
 * Groups consecutive insert operations on the same namespace and applies the combined operation
 * as a single oplog entry.
 * Advances the the std::vector<const OplogEntry*> iterator if the grouped insert is applied
 * successfully.
 */
class InsertGroup {
    InsertGroup(const InsertGroup&) = delete;
    InsertGroup& operator=(const InsertGroup&) = delete;

public:
    using ConstIterator = std::vector<const OplogEntry*>::const_iterator;
    using Mode = OplogApplication::Mode;
    typedef std::function<Status(
        OperationContext*, const OplogEntryOrGroupedInserts&, OplogApplication::Mode, bool)>
        ApplyFunc;

    InsertGroup(std::vector<const OplogEntry*>* ops,
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
    // _doNotGroupBeforePoint is used to prevent retrying bad group inserts by marking the final op
    // of a failed group and not allowing further group inserts until that op has been processed.
    ConstIterator _doNotGroupBeforePoint;

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
