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


#include "mongo/db/exec/classic/near.h"

#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {

using std::unique_ptr;

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

NearStage::NearStage(ExpressionContext* expCtx,
                     const char* typeName,
                     StageType type,
                     WorkingSet* workingSet,
                     VariantCollectionPtrOrAcquisition collection,
                     const IndexDescriptor* indexDescriptor)
    : RequiresIndexStage(typeName, expCtx, collection, indexDescriptor, workingSet),
      _workingSet(workingSet),
      _searchState(SearchState_Initializing),
      _nextIntervalStats(nullptr),
      _stageType(type),
      _nextInterval(nullptr) {}

NearStage::~NearStage() {}

NearStage::CoveredInterval::CoveredInterval(PlanStage* covering,
                                            double minDistance,
                                            double maxDistance,
                                            bool inclusiveMax)
    : covering(covering),
      minDistance(minDistance),
      maxDistance(maxDistance),
      inclusiveMax(inclusiveMax) {}


PlanStage::StageState NearStage::initNext(WorkingSetID* out) {
    PlanStage::StageState state = initialize(opCtx(), _workingSet, out);
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
    PlanStage::StageState nextState = PlanStage::NEED_TIME;

    //
    // Work the search
    //

    if (SearchState_Initializing == _searchState) {
        nextState = initNext(&toReturn);
    } else if (SearchState_Buffering == _searchState) {
        nextState = bufferNext(&toReturn);
    } else if (SearchState_Advancing == _searchState) {
        nextState = advanceNext(&toReturn);
    } else {
        invariant(SearchState_Finished == _searchState);
        nextState = PlanStage::IS_EOF;
    }

    //
    // Handle the results
    //

    if (PlanStage::ADVANCED == nextState) {
        *out = toReturn;
    } else if (PlanStage::NEED_YIELD == nextState) {
        *out = toReturn;
    } else if (PlanStage::IS_EOF == nextState) {
        _commonStats.isEOF = true;
    }

    return nextState;
}

// Set "toReturn" when NEED_YIELD.
PlanStage::StageState NearStage::bufferNext(WorkingSetID* toReturn) {
    //
    // Try to retrieve the next covered member
    //

    if (!_nextInterval) {
        auto interval = nextInterval(opCtx(), _workingSet);
        if (!interval) {
            _searchState = SearchState_Finished;
            return PlanStage::IS_EOF;
        }

        // CoveredInterval and its child stage are owned by _childrenIntervals
        _childrenIntervals.push_back(std::move(interval));
        _nextInterval = _childrenIntervals.back().get();
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
    if (nextMember->hasRecordId()) {
        if (_seenDocuments.end() != _seenDocuments.find(nextMember->recordId)) {
            _workingSet->free(nextMemberID);
            return PlanStage::NEED_TIME;
        }
    }

    ++_nextIntervalStats->numResultsBuffered;

    // If the member's distance is in the current distance interval, add it to our buffered
    // results.
    auto memberDistance = computeDistance(nextMember);

    // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
    nextMember->makeObjOwnedIfNeeded();
    _resultBuffer.push(SearchResult(nextMemberID, memberDistance));

    // Store the member's RecordId, if available, for deduping.
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

    *toReturn = resultID;

    // If we're returning something, take it out of our RecordId -> WSID map. This keeps
    // '_seenDocuments' in sync with '_resultBuffer'.
    WorkingSetMember* member = _workingSet->get(*toReturn);
    if (member->hasRecordId()) {
        _seenDocuments.erase(member->recordId);
    }

    // This value is used by nextInterval() to determine the size of the next interval.
    ++_nextIntervalStats->numResultsReturned;

    return PlanStage::ADVANCED;
}

bool NearStage::isEOF() const {
    return SearchState_Finished == _searchState;
}

unique_ptr<PlanStageStats> NearStage::getStats() {
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, _stageType);
    ret->specific = _specificStats.clone();
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
