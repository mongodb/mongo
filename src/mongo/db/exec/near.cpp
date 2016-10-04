/**
 *    Copyright (C) 2014 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/near.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

NearStage::NearStage(OperationContext* txn,
                     const char* typeName,
                     StageType type,
                     WorkingSet* workingSet,
                     Collection* collection)
    : PlanStage(typeName, txn),
      _workingSet(workingSet),
      _collection(collection),
      _searchState(SearchState_Initializing),
      _nextIntervalStats(nullptr),
      _stageType(type),
      _nextInterval(nullptr) {}

NearStage::~NearStage() {}

NearStage::CoveredInterval::CoveredInterval(PlanStage* covering,
                                            bool dedupCovering,
                                            double minDistance,
                                            double maxDistance,
                                            bool inclusiveMax)
    : covering(covering),
      dedupCovering(dedupCovering),
      minDistance(minDistance),
      maxDistance(maxDistance),
      inclusiveMax(inclusiveMax) {}


PlanStage::StageState NearStage::initNext(WorkingSetID* out) {
    PlanStage::StageState state = initialize(getOpCtx(), _workingSet, _collection, out);
    if (state == PlanStage::IS_EOF) {
        _searchState = SearchState_Buffering;
        return PlanStage::NEED_TIME;
    }

    invariant(state != PlanStage::ADVANCED);

    // Propagate NEED_TIME or errors upward.
    return state;
}

PlanStage::StageState NearStage::doWork(WorkingSetID* out) {
    WorkingSetID toReturn = WorkingSet::INVALID_ID;
    Status error = Status::OK();
    PlanStage::StageState nextState = PlanStage::NEED_TIME;

    //
    // Work the search
    //

    if (SearchState_Initializing == _searchState) {
        nextState = initNext(&toReturn);
    } else if (SearchState_Buffering == _searchState) {
        nextState = bufferNext(&toReturn, &error);
    } else if (SearchState_Advancing == _searchState) {
        nextState = advanceNext(&toReturn);
    } else {
        invariant(SearchState_Finished == _searchState);
        nextState = PlanStage::IS_EOF;
    }

    //
    // Handle the results
    //

    if (PlanStage::FAILURE == nextState) {
        *out = WorkingSetCommon::allocateStatusMember(_workingSet, error);
    } else if (PlanStage::ADVANCED == nextState) {
        *out = toReturn;
    } else if (PlanStage::NEED_YIELD == nextState) {
        *out = toReturn;
    } else if (PlanStage::IS_EOF == nextState) {
        _commonStats.isEOF = true;
    }

    return nextState;
}

/**
 * Holds a generic search result with a distance computed in some fashion.
 */
struct NearStage::SearchResult {
    SearchResult(WorkingSetID resultID, double distance) : resultID(resultID), distance(distance) {}

    bool operator<(const SearchResult& other) const {
        // We want increasing distance, not decreasing, so we reverse the <
        return distance > other.distance;
    }

    WorkingSetID resultID;
    double distance;
};

// Set "toReturn" when NEED_YIELD.
PlanStage::StageState NearStage::bufferNext(WorkingSetID* toReturn, Status* error) {
    //
    // Try to retrieve the next covered member
    //

    if (!_nextInterval) {
        StatusWith<CoveredInterval*> intervalStatus =
            nextInterval(getOpCtx(), _workingSet, _collection);
        if (!intervalStatus.isOK()) {
            _searchState = SearchState_Finished;
            *error = intervalStatus.getStatus();
            return PlanStage::FAILURE;
        }

        if (NULL == intervalStatus.getValue()) {
            _searchState = SearchState_Finished;
            return PlanStage::IS_EOF;
        }

        // CoveredInterval and its child stage are owned by _childrenIntervals
        _childrenIntervals.push_back(intervalStatus.getValue());
        _nextInterval = _childrenIntervals.back();
        _specificStats.intervalStats.emplace_back();
        _nextIntervalStats = &_specificStats.intervalStats.back();
        _nextIntervalStats->minDistanceAllowed = _nextInterval->minDistance;
        _nextIntervalStats->maxDistanceAllowed = _nextInterval->maxDistance;
        _nextIntervalStats->inclusiveMaxDistanceAllowed = _nextInterval->inclusiveMax;
    }

    WorkingSetID nextMemberID;
    PlanStage::StageState intervalState = _nextInterval->covering->work(&nextMemberID);

    if (PlanStage::IS_EOF == intervalState) {
        _searchState = SearchState_Advancing;
        return PlanStage::NEED_TIME;
    } else if (PlanStage::FAILURE == intervalState) {
        *error = WorkingSetCommon::getMemberStatus(*_workingSet->get(nextMemberID));
        return intervalState;
    } else if (PlanStage::NEED_YIELD == intervalState) {
        *toReturn = nextMemberID;
        return intervalState;
    } else if (PlanStage::ADVANCED != intervalState) {
        return intervalState;
    }

    //
    // Try to buffer the next covered member
    //

    WorkingSetMember* nextMember = _workingSet->get(nextMemberID);

    // The child stage may not dedup so we must dedup them ourselves.
    if (_nextInterval->dedupCovering && nextMember->hasRecordId()) {
        if (_seenDocuments.end() != _seenDocuments.find(nextMember->recordId)) {
            _workingSet->free(nextMemberID);
            return PlanStage::NEED_TIME;
        }
    }

    ++_nextIntervalStats->numResultsBuffered;

    StatusWith<double> distanceStatus = computeDistance(nextMember);

    if (!distanceStatus.isOK()) {
        _searchState = SearchState_Finished;
        *error = distanceStatus.getStatus();
        return PlanStage::FAILURE;
    }

    // If the member's distance is in the current distance interval, add it to our buffered
    // results.
    double memberDistance = distanceStatus.getValue();

    // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
    nextMember->makeObjOwnedIfNeeded();
    _resultBuffer.push(SearchResult(nextMemberID, memberDistance));

    // Store the member's RecordId, if available, for quick invalidation
    if (nextMember->hasRecordId()) {
        _seenDocuments.insert(std::make_pair(nextMember->recordId, nextMemberID));
    }

    return PlanStage::NEED_TIME;
}

PlanStage::StageState NearStage::advanceNext(WorkingSetID* toReturn) {
    // Returns documents to the parent stage.
    // If the document does not fall in the current interval, it will be buffered so that
    // it might be returned in a following interval.

    // Check if the next member is in the search interval and that the buffer isn't empty
    WorkingSetID resultID = WorkingSet::INVALID_ID;
    // memberDistance is initialized to produce an error if used before its value is changed
    double memberDistance = std::numeric_limits<double>::lowest();
    if (!_resultBuffer.empty()) {
        SearchResult result = _resultBuffer.top();
        memberDistance = result.distance;

        // Throw out all documents with memberDistance < minDistance
        if (memberDistance < _nextInterval->minDistance) {
            WorkingSetMember* member = _workingSet->get(result.resultID);
            if (member->hasRecordId()) {
                _seenDocuments.erase(member->recordId);
            }
            _resultBuffer.pop();
            _workingSet->free(result.resultID);
            return PlanStage::NEED_TIME;
        }

        bool inInterval = _nextInterval->inclusiveMax ? memberDistance <= _nextInterval->maxDistance
                                                      : memberDistance < _nextInterval->maxDistance;
        if (inInterval) {
            resultID = result.resultID;
        }
    } else {
        // A document should be in _seenDocuments if and only if it's in _resultBuffer
        invariant(_seenDocuments.empty());
    }

    // memberDistance is not in the interval or _resultBuffer is empty,
    // so we need to move to the next interval.
    if (WorkingSet::INVALID_ID == resultID) {
        _nextInterval = nullptr;
        _nextIntervalStats = nullptr;
        _searchState = SearchState_Buffering;
        return PlanStage::NEED_TIME;
    }

    // The next document in _resultBuffer is in the search interval, so we can return it.
    _resultBuffer.pop();

    // If we're returning something, take it out of our RecordId -> WSID map so that future
    // calls to invalidate don't cause us to take action for a RecordId we're done with.
    *toReturn = resultID;
    WorkingSetMember* member = _workingSet->get(*toReturn);
    if (member->hasRecordId()) {
        _seenDocuments.erase(member->recordId);
    }

    // This value is used by nextInterval() to determine the size of the next interval.
    ++_nextIntervalStats->numResultsReturned;

    return PlanStage::ADVANCED;
}

bool NearStage::isEOF() {
    return SearchState_Finished == _searchState;
}

void NearStage::doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    // If a result is in _resultBuffer and has a RecordId it will be in _seenDocuments as
    // well. It's safe to return the result w/o the RecordId, so just fetch the result.
    unordered_map<RecordId, WorkingSetID, RecordId::Hasher>::iterator seenIt =
        _seenDocuments.find(dl);

    if (seenIt != _seenDocuments.end()) {
        WorkingSetMember* member = _workingSet->get(seenIt->second);
        verify(member->hasRecordId());
        WorkingSetCommon::fetchAndInvalidateRecordId(txn, member, _collection);
        verify(!member->hasRecordId());

        // Don't keep it around in the seen map since there's no valid RecordId anymore
        _seenDocuments.erase(seenIt);
    }
}

unique_ptr<PlanStageStats> NearStage::getStats() {
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, _stageType);
    ret->specific.reset(_specificStats.clone());
    for (size_t i = 0; i < _childrenIntervals.size(); ++i) {
        ret->children.emplace_back(_childrenIntervals[i]->covering->getStats());
    }
    return ret;
}

StageType NearStage::stageType() const {
    return _stageType;
}

const SpecificStats* NearStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
