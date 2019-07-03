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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/sort.h"

#include <algorithm>
#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::unique_ptr;
using std::vector;

// static
const char* SortStage::kStageType = "SORT";

SortStage::WorkingSetComparator::WorkingSetComparator(BSONObj p) : pattern(p) {}

bool SortStage::WorkingSetComparator::operator()(const SortableDataItem& lhs,
                                                 const SortableDataItem& rhs) const {
    // False means ignore field names.
    int result = lhs.sortKey.woCompare(rhs.sortKey, pattern, false);
    if (0 != result) {
        return result < 0;
    }
    // Indices use RecordId as an additional sort key so we must as well.
    return lhs.recordId < rhs.recordId;
}

SortStage::SortStage(OperationContext* opCtx,
                     const SortStageParams& params,
                     WorkingSet* ws,
                     PlanStage* child)
    : PlanStage(kStageType, opCtx),
      _ws(ws),
      _pattern(params.pattern),
      _limit(params.limit),
      _allowDiskUse(params.allowDiskUse),
      _sorted(false),
      _resultIterator(_data.end()),
      _memUsage(0) {
    _children.emplace_back(child);

    BSONObj sortComparator = FindCommon::transformSortSpec(_pattern);
    _sortKeyComparator = std::make_unique<WorkingSetComparator>(sortComparator);

    // If limit > 1, we need to initialize _dataSet here to maintain ordered set of data items while
    // fetching from the child stage.
    if (_limit > 1) {
        const WorkingSetComparator& cmp = *_sortKeyComparator;
        _dataSet.reset(new SortableDataItemSet(cmp));
    }
}

SortStage::~SortStage() {}

bool SortStage::isEOF() {
    // We're done when our child has no more results, we've sorted the child's results, and
    // we've returned all sorted results.
    return child()->isEOF() && _sorted && (_data.end() == _resultIterator);
}

PlanStage::StageState SortStage::doWork(WorkingSetID* out) {
    const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes.load());
    if (_memUsage > maxBytes) {
        str::stream ss;
        ss << "Sort operation used more than the maximum " << maxBytes
           << " bytes of RAM. Add an index, or specify a smaller limit.";
        Status status(ErrorCodes::OperationFailed, ss);
        *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        return PlanStage::FAILURE;
    }

    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // Still reading in results to sort.
    if (!_sorted) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState code = child()->work(&id);

        if (PlanStage::ADVANCED == code) {
            WorkingSetMember* member = _ws->get(id);

            SortableDataItem item;
            item.wsid = id;

            // We extract the sort key from the WSM's computed data. This must have been generated
            // by a SortKeyGeneratorStage descendent in the execution tree.
            auto sortKeyComputedData =
                static_cast<const SortKeyComputedData*>(member->getComputed(WSM_SORT_KEY));
            item.sortKey = sortKeyComputedData->getSortKey();

            if (member->hasRecordId()) {
                // The RecordId breaks ties when sorting two WSMs with the same sort key.
                item.recordId = member->recordId;
            }

            addToBuffer(item);

            return PlanStage::NEED_TIME;
        } else if (PlanStage::IS_EOF == code) {
            // TODO: We don't need the lock for this.  We could ask for a yield and do this work
            // unlocked.  Also, this is performing a lot of work for one call to work(...)
            sortBuffer();
            _resultIterator = _data.begin();
            _sorted = true;
            return PlanStage::NEED_TIME;
        } else if (PlanStage::FAILURE == code) {
            // The stage which produces a failure is responsible for allocating a working set member
            // with error details.
            invariant(WorkingSet::INVALID_ID != id);
            *out = id;
            return code;
        } else if (PlanStage::NEED_YIELD == code) {
            *out = id;
        }

        return code;
    }

    // Returning results.
    verify(_resultIterator != _data.end());
    verify(_sorted);
    *out = _resultIterator->wsid;
    _resultIterator++;

    return PlanStage::ADVANCED;
}

unique_ptr<PlanStageStats> SortStage::getStats() {
    _commonStats.isEOF = isEOF();
    const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes.load());
    _specificStats.memLimit = maxBytes;
    _specificStats.memUsage = _memUsage;
    _specificStats.limit = _limit;
    _specificStats.sortPattern = _pattern.getOwned();

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_SORT);
    ret->specific = std::make_unique<SortStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SortStage::getSpecificStats() const {
    return &_specificStats;
}

/**
 * addToBuffer() and sortBuffer() work differently based on the
 * configured limit. addToBuffer() is also responsible for
 * performing some accounting on the overall memory usage to
 * make sure we're not using too much memory.
 *
 * limit == 0:
 *     addToBuffer() - Adds item to vector.
 *     sortBuffer() - Sorts vector.
 * limit == 1:
 *     addToBuffer() - Replaces first item in vector with max of
 *                     current and new item.
 *                     Updates memory usage if item was replaced.
 *     sortBuffer() - Does nothing.
 * limit > 1:
 *     addToBuffer() - Does not update vector. Adds item to set.
 *                     If size of set exceeds limit, remove item from set
 *                     with lowest key. Updates memory usage accordingly.
 *     sortBuffer() - Copies items from set to vectors.
 */
void SortStage::addToBuffer(const SortableDataItem& item) {
    // Holds ID of working set member to be freed at end of this function.
    WorkingSetID wsidToFree = WorkingSet::INVALID_ID;

    WorkingSetMember* member = _ws->get(item.wsid);
    if (_limit == 0) {
        // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
        member->makeObjOwnedIfNeeded();
        _data.push_back(item);
        _memUsage += member->getMemUsage();
    } else if (_limit == 1) {
        if (_data.empty()) {
            member->makeObjOwnedIfNeeded();
            _data.push_back(item);
            _memUsage = member->getMemUsage();
            return;
        }
        wsidToFree = item.wsid;
        const WorkingSetComparator& cmp = *_sortKeyComparator;
        // Compare new item with existing item in vector.
        if (cmp(item, _data[0])) {
            wsidToFree = _data[0].wsid;
            member->makeObjOwnedIfNeeded();
            _data[0] = item;
            _memUsage = member->getMemUsage();
        }
    } else {
        // Update data item set instead of vector
        // Limit not reached - insert and return
        vector<SortableDataItem>::size_type limit(_limit);
        if (_dataSet->size() < limit) {
            member->makeObjOwnedIfNeeded();
            _dataSet->insert(item);
            _memUsage += member->getMemUsage();
            return;
        }
        // Limit will be exceeded - compare with item with lowest key
        // If new item does not have a lower key value than last item,
        // do nothing.
        wsidToFree = item.wsid;
        SortableDataItemSet::const_iterator lastItemIt = --(_dataSet->end());
        const SortableDataItem& lastItem = *lastItemIt;
        const WorkingSetComparator& cmp = *_sortKeyComparator;
        if (cmp(item, lastItem)) {
            _memUsage -= _ws->get(lastItem.wsid)->getMemUsage();
            _memUsage += member->getMemUsage();
            wsidToFree = lastItem.wsid;
            // According to std::set iterator validity rules,
            // it does not matter which of erase()/insert() happens first.
            // Here, we choose to erase first to release potential resources
            // used by the last item and to keep the scope of the iterator to a minimum.
            _dataSet->erase(lastItemIt);
            member->makeObjOwnedIfNeeded();
            _dataSet->insert(item);
        }
    }

    // There was a buffered result which we can throw out because we are executing a sort with a
    // limit, and the result is now known not to be in the top k set. Free the working set member
    // associated with 'wsidToFree'.
    if (wsidToFree != WorkingSet::INVALID_ID) {
        _ws->free(wsidToFree);
    }
}

void SortStage::sortBuffer() {
    if (_limit == 0) {
        const WorkingSetComparator& cmp = *_sortKeyComparator;
        std::sort(_data.begin(), _data.end(), cmp);
    } else if (_limit == 1) {
        // Buffer contains either 0 or 1 item so it is already in a sorted state.
        return;
    } else {
        // Set already contains items in sorted order, so we simply copy the items
        // from the set to the vector.
        // Release the memory for the set after the copy.
        vector<SortableDataItem> newData(_dataSet->begin(), _dataSet->end());
        _data.swap(newData);
        _dataSet.reset();
    }
}

}  // namespace mongo
