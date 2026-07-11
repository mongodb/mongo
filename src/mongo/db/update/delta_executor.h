// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo {

/**
 * An UpdateExecutor representing a delta-style update. Delta-style updates apply a diff
 * to the pre image document in order to recover the post image.
 */
class DeltaExecutor : public UpdateExecutor {
public:
    /**
     * Initializes the executor with the diff to apply.
     */
    explicit DeltaExecutor(doc_diff::Diff diff, bool mustCheckExistenceForInsertOperations)
        : _diff(std::move(diff)),
          _outputOplogEntry(update_oplog_entry::makeDeltaOplogEntry(_diff)),
          _mustCheckExistenceForInsertOperations(mustCheckExistenceForInsertOperations) {}

    ApplyResult applyUpdate(ApplyParams applyParams) const final;

    Value serialize() const final {
        // Delta updates are only applied internally on secondaries. They are never passed between
        // nodes or re-written.
        MONGO_UNREACHABLE;
    }

private:
    doc_diff::Diff _diff;

    // Although the delta executor is only used for applying $v:2 oplog entries on secondaries, it
    // still needs to produce an oplog entry from the applyUpdate() method so that OpObservers may
    // handle the event appropriately.
    BSONObj _outputOplogEntry;

    const bool _mustCheckExistenceForInsertOperations;
};

}  // namespace mongo
