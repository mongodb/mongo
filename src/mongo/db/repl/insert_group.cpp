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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/insert_group.h"

#include <algorithm>
#include <iterator>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

// Must not create too large an object.
const auto kInsertGroupMaxBatchSize = write_ops::insertVectorMaxBytes;

// Limit number of ops in a single group.
constexpr auto kInsertGroupMaxBatchCount = 64;

}  // namespace

InsertGroup::InsertGroup(MultiApplier::OperationPtrs* ops,
                         OperationContext* opCtx,
                         InsertGroup::Mode mode)
    : _doNotGroupBeforePoint(ops->cbegin()), _end(ops->cend()), _opCtx(opCtx), _mode(mode) {}

StatusWith<InsertGroup::ConstIterator> InsertGroup::groupAndApplyInserts(ConstIterator it) {
    const auto& entry = **it;

    // The following conditions must be met before attempting to group the oplog entries starting
    // at 'oplogEntriesIterator':
    // 1) The CRUD operation must an insert;
    // 2) The namespace that we are inserting into cannot be a capped collection;
    // 3) We have not attempted to group this insert during a previous call to this function.
    if (entry.getOpType() != OpTypeEnum::kInsert) {
        return Status(ErrorCodes::TypeMismatch, "Can only group insert operations.");
    }
    if (entry.isForCappedCollection) {
        return Status(ErrorCodes::InvalidOptions,
                      "Cannot group insert operations on capped collections.");
    }
    if (it <= _doNotGroupBeforePoint) {
        return Status(ErrorCodes::InvalidPath,
                      "Cannot group an insert operation that we previously attempted to group.");
    }

    // Attempt to group 'insert' ops if possible.
    std::vector<BSONObj> toInsert;

    // Make sure to include the first op in the batch size.
    size_t batchSize = entry.getObject().objsize();
    auto batchCount = MultiApplier::OperationPtrs::size_type(1);
    auto batchNamespace = entry.getNss();

    /**
     * Search for the op that delimits this insert batch, and save its position
     * in endOfGroupableOpsIterator. For example, given the following list of oplog
     * entries with a sequence of groupable inserts:
     *
     *                S--------------E
     *       u, u, u, i, i, i, i, i, d, d
     *
     *       S: start of insert group
     *       E: end of groupable ops
     *
     * E is the position of endOfGroupableOpsIterator. i.e. endOfGroupableOpsIterator
     * will point to the first op that *can't* be added to the current insert group.
     */
    auto endOfGroupableOpsIterator =
        std::find_if(it + 1, _end, [&](const OplogEntry* nextEntry) -> bool {
            auto opNamespace = nextEntry->getNss();
            batchSize += nextEntry->getObject().objsize();
            batchCount += 1;

            // Only add the op to this batch if it passes the criteria.
            return nextEntry->getOpType() != OpTypeEnum::kInsert  // Must be an insert.
                || opNamespace != batchNamespace                  // Must be in the same namespace.
                || batchSize > kInsertGroupMaxBatchSize  // Must not create too large an object.
                ||
                batchCount > kInsertGroupMaxBatchCount;  // Limit number of ops in a single group.
        });

    // See if we were able to create a group that contains more than a single op.
    if (std::distance(it, endOfGroupableOpsIterator) == 1) {
        return Status(ErrorCodes::NoSuchKey,
                      "Not able to create a group with more than a single insert operation");
    }

    // Create an oplog entry batch for grouped inserts.
    OplogEntryBatch groupedInsertBatch(it, endOfGroupableOpsIterator);
    try {
        // Apply the group of inserts by passing in groupedInsertBatch.
        uassertStatusOK(applyOplogEntryBatch(_opCtx, groupedInsertBatch, _mode));
        // It succeeded, advance the oplogEntriesIterator to the end of the
        // group of inserts.
        return endOfGroupableOpsIterator - 1;
    } catch (...) {
        // The group insert failed, log an error and fall through to the
        // application of an individual op.
        auto status = exceptionToStatus().withContext(
            str::stream() << "Error applying inserts in bulk: "
                          << redact(groupedInsertBatch.toBSON())
                          << ". Trying first insert as a lone insert: " << redact(entry.getRaw()));

        // It's not an error during initial sync to encounter DuplicateKey errors.
        if (Mode::kInitialSync == _mode && ErrorCodes::DuplicateKey == status) {
            LOG(2) << status;
        } else {
            error() << status;
        }

        // Avoid quadratic run time from failed insert by not retrying until we
        // are beyond this group of ops.
        _doNotGroupBeforePoint = endOfGroupableOpsIterator - 1;

        return status;
    }

    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
