/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/applier_helpers.h"

#include <algorithm>
#include <iterator>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

// Must not create too large an object.
const auto kInsertGroupMaxBatchSize = insertVectorMaxBytes;

// Limit number of ops in a single group.
constexpr auto kInsertGroupMaxBatchCount = 64;

}  // namespace

// static
void ApplierHelpers::stableSortByNamespace(MultiApplier::OperationPtrs* oplogEntryPointers) {
    if (oplogEntryPointers->size() < 1U) {
        return;
    }
    auto nssComparator = [](const OplogEntry* l, const OplogEntry* r) {
        return l->getNamespace() < r->getNamespace();
    };
    std::stable_sort(oplogEntryPointers->begin(), oplogEntryPointers->end(), nssComparator);
}

using InsertGroup = ApplierHelpers::InsertGroup;

InsertGroup::InsertGroup(ApplierHelpers::OperationPtrs* ops,
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
    auto batchSize = entry.getObject().objsize();
    auto batchCount = OperationPtrs::size_type(1);
    auto batchNamespace = entry.getNamespace();

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
            auto opNamespace = nextEntry->getNamespace();
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

    // Since we found more than one document, create grouped insert of many docs.
    // We are going to group many 'i' ops into one big 'i' op, with array fields for
    // 'ts', 't', and 'o', corresponding to each individual op.
    // For example:
    // { ts: Timestamp(1,1), t:1, ns: "test.foo", op:"i", o: {_id:1} }
    // { ts: Timestamp(1,2), t:1, ns: "test.foo", op:"i", o: {_id:2} }
    // become:
    // { ts: [Timestamp(1, 1), Timestamp(1, 2)],
    //    t: [1, 1],
    //    o: [{_id: 1}, {_id: 2}],
    //   ns: "test.foo",
    //   op: "i" }
    BSONObjBuilder groupedInsertBuilder;

    // Populate the "ts" field with an array of all the grouped inserts' timestamps.
    {
        BSONArrayBuilder tsArrayBuilder(groupedInsertBuilder.subarrayStart("ts"));
        for (auto groupingIt = it; groupingIt != endOfGroupableOpsIterator; ++groupingIt) {
            tsArrayBuilder.append((*groupingIt)->getTimestamp());
        }
    }

    // Populate the "t" (term) field with an array of all the grouped inserts' terms.
    {
        BSONArrayBuilder tArrayBuilder(groupedInsertBuilder.subarrayStart("t"));
        for (auto groupingIt = it; groupingIt != endOfGroupableOpsIterator; ++groupingIt) {
            auto parsedTerm = (*groupingIt)->getTerm();
            long long term = OpTime::kUninitializedTerm;
            // Term may not be present (pv0)
            if (parsedTerm) {
                term = parsedTerm.get();
            }
            tArrayBuilder.append(term);
        }
    }

    // Populate the "o" field with an array of all the grouped inserts.
    {
        BSONArrayBuilder oArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
        for (auto groupingIt = it; groupingIt != endOfGroupableOpsIterator; ++groupingIt) {
            oArrayBuilder.append((*groupingIt)->getObject());
        }
    }

    // Generate an op object of all elements except for "ts", "t", and "o", since we
    // need to make those fields arrays of all the ts's, t's, and o's.
    groupedInsertBuilder.appendElementsUnique(entry.raw);

    auto groupedInsertObj = groupedInsertBuilder.done();
    try {
        // Apply the group of inserts.
        uassertStatusOK(SyncTail::syncApply(_opCtx, groupedInsertObj, _mode));
        // It succeeded, advance the oplogEntriesIterator to the end of the
        // group of inserts.
        return endOfGroupableOpsIterator - 1;
    } catch (...) {
        // The group insert failed, log an error and fall through to the
        // application of an individual op.
        auto status = exceptionToStatus().withContext(
            str::stream() << "Error applying inserts in bulk: " << redact(groupedInsertObj)
                          << ". Trying first insert as a lone insert: "
                          << redact(entry.raw));
        error() << status;

        // Avoid quadratic run time from failed insert by not retrying until we
        // are beyond this group of ops.
        _doNotGroupBeforePoint = endOfGroupableOpsIterator - 1;

        return status;
    }

    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
