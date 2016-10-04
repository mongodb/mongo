/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/sort.h"

#include <algorithm>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

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
      _collection(params.collection),
      _ws(ws),
      _pattern(params.pattern),
      _limit(params.limit),
      _sorted(false),
      _resultIterator(_data.end()),
      _memUsage(0) {
    _children.emplace_back(child);

    BSONObj sortComparator = FindCommon::transformSortSpec(_pattern);
    _sortKeyComparator = stdx::make_unique<WorkingSetComparator>(sortComparator);

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
    const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes);
    if (_memUsage > maxBytes) {
        mongoutils::str::stream ss;
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
            // Add it into the map for quick invalidation if it has a valid RecordId.
            // A RecordId may be invalidated at any time (during a yield).  We need to get into
            // the WorkingSet as quickly as possible to handle it.
            WorkingSetMember* member = _ws->get(id);

            // Planner must put a fetch before we get here.
            verify(member->hasObj());

            // We might be sorting something that was invalidated at some point.
            if (member->hasRecordId()) {
                _wsidByRecordId[member->recordId] = id;
            }

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
        } else if (PlanStage::FAILURE == code || PlanStage::DEAD == code) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "sort stage failed to read in results to sort from child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember(_ws, status);
            }
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

    // If we're returning something, take it out of our DL -> WSID map so that future
    // calls to invalidate don't cause us to take action for a DL we're done with.
    WorkingSetMember* member = _ws->get(*out);
    if (member->hasRecordId()) {
        _wsidByRecordId.erase(member->recordId);
    }

    return PlanStage::ADVANCED;
}

void SortStage::doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    // If we have a deletion, we can fetch and carry on.
    // If we have a mutation, it's easier to fetch and use the previous document.
    // So, no matter what, fetch and keep the doc in play.

    // _data contains indices into the WorkingSet, not actual data.  If a WorkingSetMember in
    // the WorkingSet needs to change state as a result of a RecordId invalidation, it will still
    // be at the same spot in the WorkingSet.  As such, we don't need to modify _data.
    DataMap::iterator it = _wsidByRecordId.find(dl);

    // If we're holding on to data that's got the RecordId we're invalidating...
    if (_wsidByRecordId.end() != it) {
        // Grab the WSM that we're nuking.
        WorkingSetMember* member = _ws->get(it->second);
        verify(member->recordId == dl);

        WorkingSetCommon::fetchAndInvalidateRecordId(txn, member, _collection);

        // Remove the RecordId from our set of active DLs.
        _wsidByRecordId.erase(it);
        ++_specificStats.forcedFetches;
    }
}

unique_ptr<PlanStageStats> SortStage::getStats() {
    _commonStats.isEOF = isEOF();
    const size_t maxBytes = static_cast<size_t>(internalQueryExecMaxBlockingSortBytes);
    _specificStats.memLimit = maxBytes;
    _specificStats.memUsage = _memUsage;
    _specificStats.limit = _limit;
    _specificStats.sortPattern = _pattern.getOwned();

    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_SORT);
    ret->specific = make_unique<SortStats>(_specificStats);
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

    // If the working set ID is valid, remove from
    // RecordId invalidation map and free from working set.
    if (wsidToFree != WorkingSet::INVALID_ID) {
        WorkingSetMember* member = _ws->get(wsidToFree);
        if (member->hasRecordId()) {
            _wsidByRecordId.erase(member->recordId);
        }
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
