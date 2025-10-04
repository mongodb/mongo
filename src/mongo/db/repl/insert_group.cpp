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


#include "mongo/db/repl/insert_group.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

// Must not create too large an object.
const auto kInsertGroupMaxGroupSize = write_ops::insertVectorMaxBytes;

// Limit number of ops in a single group.
constexpr auto kInsertGroupMaxOpCount = 64;

}  // namespace

InsertGroup::InsertGroup(std::vector<ApplierOperation>* ops,
                         OperationContext* opCtx,
                         InsertGroup::Mode mode,
                         const bool isDataConsistent,
                         ApplyFunc applyOplogEntryOrGroupedInserts)
    : _nextOpToGroup(ops->cbegin()),
      _end(ops->cend()),
      _opCtx(opCtx),
      _mode(mode),
      _isDataConsistent(isDataConsistent),
      _applyOplogEntryOrGroupedInserts(applyOplogEntryOrGroupedInserts) {}

StatusWith<InsertGroup::ConstIterator> InsertGroup::groupAndApplyInserts(
    ConstIterator it) noexcept {
    const auto& op = *it;

    // The following conditions must be met before attempting to group the oplog entries starting
    // at 'oplogEntriesIterator':
    // 1) The CRUD operation must an insert;
    // 2) The namespace that we are inserting into cannot be a capped collection;
    // 3) We have not attempted to group this insert during a previous call to this function.
    if (op->getOpType() != OpTypeEnum::kInsert) {
        return Status(ErrorCodes::TypeMismatch, "Can only group insert operations.");
    }
    if (op->isForCappedCollection()) {
        return Status(ErrorCodes::InvalidOptions,
                      "Cannot group insert operations on capped collections.");
    }
    if (it < _nextOpToGroup) {
        return Status(ErrorCodes::InvalidPath,
                      "Cannot group an insert operation that we previously attempted to group.");
    }

    // Attempt to group 'insert' ops if possible.
    // Make sure to include the first op in the group size.
    size_t groupSize = op->getObject().objsize();
    auto opCount = std::vector<ApplierOperation>::size_type(1);
    auto groupNamespace = op->getNss();
    auto& groupVCtx = op->getVersionContext();

    /**
     * Search for the op that delimits this insert group, and save its position
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
        std::find_if(it + 1, _end, [&](const ApplierOperation& nextOp) -> bool {
            auto opNamespace = nextOp->getNss();
            groupSize += nextOp->getObject().objsize();
            opCount += 1;

            // Only add the op to this group if it passes the criteria.
            return nextOp->getOpType() != OpTypeEnum::kInsert  // Must be an insert.
                || opNamespace != groupNamespace               // Must be in the same namespace.
                || nextOp->getVersionContext() != groupVCtx    // Must be in the same Operation FCV.
                || groupSize > kInsertGroupMaxGroupSize  // Must not create too large an object.
                || opCount > kInsertGroupMaxOpCount;     // Limit number of ops in a single group.
        });

    // See if we were able to create a group that contains more than a single op.
    if (std::distance(it, endOfGroupableOpsIterator) == 1) {
        return Status(ErrorCodes::NoSuchKey,
                      "Not able to create a group with more than a single insert operation");
    }

    // Create an oplog entry group for grouped inserts.
    OplogEntryOrGroupedInserts groupedInserts(it, endOfGroupableOpsIterator);
    try {
        uassertStatusOK(
            _applyOplogEntryOrGroupedInserts(_opCtx, groupedInserts, _mode, _isDataConsistent));
        // It succeeded, advance the oplogEntriesIterator to the end of the
        // group of inserts.
        return endOfGroupableOpsIterator - 1;
    } catch (...) {
        // The group insert failed, log an error and fall through to the
        // application of an individual op.
        static constexpr char message[] =
            "Error applying inserts in bulk. Trying first insert as a lone insert";
        auto status = exceptionToStatus();

        // It's not an error during initial sync to encounter DuplicateKey errors.
        if (Mode::kInitialSync == _mode &&
            (ErrorCodes::DuplicateKey == status || ErrorCodes::NamespaceNotFound == status)) {
            LOGV2_DEBUG(21203,
                        2,
                        message,
                        "error"_attr = redact(status),
                        "groupedInserts"_attr = redact(groupedInserts.toBSON()),
                        "firstInsert"_attr = redact(op->toBSONForLogging()));
        } else {
            LOGV2_ERROR(21204,
                        message,
                        "error"_attr = redact(status),
                        "groupedInserts"_attr = redact(groupedInserts.toBSON()),
                        "firstInsert"_attr = redact(op->toBSONForLogging()));
        }

        // Avoid quadratic run time from failed insert by not retrying until we
        // are beyond this group of ops.
        _nextOpToGroup = endOfGroupableOpsIterator;

        return status;
    }

    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
