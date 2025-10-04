/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/assert_util.h"

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
