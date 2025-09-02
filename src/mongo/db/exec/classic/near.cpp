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

#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"

#include <limits>
#include <memory>

namespace mongo {

NearStage::NearStage(ExpressionContext* expCtx,
                     const char* typeName,
                     StageType type,
                     WorkingSet* workingSet,
                     VariantCollectionPtrOrAcquisition collection,
                     const IndexDescriptor* indexDescriptor)
    : RequiresIndexStage(typeName, expCtx, collection, indexDescriptor, workingSet),
      _workingSet(workingSet),
      _searchState(SearchState::Initializing),
      _seenDocuments(expCtx),
      _nextIntervalStats(nullptr),
      _sorterFileStats(nullptr /*sorterTracker*/),
      _resultBuffer(makeSortOptions(), SorterKeyComparator{}, NoOpBound{}),
      _stageType(type),
      _nextInterval(nullptr) {}

NearStage::~NearStage() {}

NearStage::CoveredInterval::CoveredInterval(PlanStage* covering,
                                            double minDistance,
                                            double maxDistance,
                                            bool isLastInterval)
    : covering(covering),
      minDistance(minDistance),
      maxDistance(maxDistance),
      isLastInterval(isLastInterval) {}


PlanStage::StageState NearStage::initNext(WorkingSetID* out) {
    PlanStage::StageState state = initialize(opCtx(), _workingSet, out);
    if (state == PlanStage::IS_EOF) {
        _searchState = SearchState::Buffering;
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

    if (SearchState::Initializing == _searchState) {
        nextState = initNext(&toReturn);
    } else if (SearchState::Buffering == _searchState) {
        nextState = bufferNext(&toReturn);
    } else if (SearchState::Advancing == _searchState) {
        nextState = advanceNext(&toReturn);
    } else {
        invariant(SearchState::Finished == _searchState);
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
            _searchState = SearchState::Finished;
            return PlanStage::IS_EOF;
        }

        tassert(10922800,
                "Intervals must be continious",
                _childrenIntervals.empty() ||
                    interval->minDistance <= _childrenIntervals.back()->maxDistance +
                            std::numeric_limits<double>::epsilon());

        // CoveredInterval and its child stage are owned by _childrenIntervals
        _childrenIntervals.push_back(std::move(interval));
        _nextInterval = _childrenIntervals.back().get();

        _specificStats.intervalStats.emplace_back();
        _nextIntervalStats = &_specificStats.intervalStats.back();
        _nextIntervalStats->minDistanceAllowed = _nextInterval->minDistance;
        _nextIntervalStats->maxDistanceAllowed = _nextInterval->maxDistance;
        _nextIntervalStats->inclusiveMaxDistanceAllowed = _nextInterval->isLastInterval;
    }

    WorkingSetID nextMemberID;
    PlanStage::StageState intervalState = _nextInterval->covering->work(&nextMemberID);

    if (PlanStage::IS_EOF == intervalState) {
        if (_nextInterval->isLastInterval) {
            _resultBuffer.done();
        } else {
            _resultBuffer.setBound(SorterKey{_nextInterval->maxDistance});
        }

        _searchState = SearchState::Advancing;
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

    WorkingSetMember nextMember = _workingSet->extract(nextMemberID);

    // The child stage may not dedup so we must dedup them ourselves.
    if (nextMember.hasRecordId()) {
        if (!_seenDocuments.insert(nextMember.recordId)) {
            return PlanStage::NEED_TIME;
        }
    }

    auto memberDistance = computeDistance(&nextMember);
    if (memberDistance < _nextInterval->minDistance) {
        if (nextMember.hasRecordId()) {
            _seenDocuments.freeMemory(nextMember.recordId);
        }
        return PlanStage::NEED_TIME;
    }

    ++_nextIntervalStats->numResultsBuffered;

    // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
    nextMember.makeObjOwnedIfNeeded();
    _resultBuffer.add(SorterKey{memberDistance}, std::move(nextMember));

    if (feature_flags::gFeatureFlagExtendedAutoSpilling.isEnabled()) {
        spill(loadMemoryLimit(StageMemoryLimit::NearStageMaxMemoryBytes));
    }

    return PlanStage::NEED_TIME;
}

PlanStage::StageState NearStage::advanceNext(WorkingSetID* toReturn) {
    // Returns documents to the parent stage.
    // If the document does not fall in the current interval, it will be buffered so that
    // it might be returned in a following interval.

    // Check if the next member is in the search interval and that the buffer isn't empty
    WorkingSetID resultID = WorkingSet::INVALID_ID;
    if (_resultBuffer.getState() == ResultBufferSorter::State::kReady) {
        auto [memberDistance, member] = _resultBuffer.next();
        if (member->hasRecordId()) {
            _seenDocuments.freeMemory(member->recordId);
        }

        const bool inInterval = _nextInterval->isLastInterval
            ? memberDistance.value <= _nextInterval->maxDistance
            : memberDistance.value < _nextInterval->maxDistance;
        if (inInterval) {
            resultID = _workingSet->emplace(member.extract());
        }
    }

    // memberDistance is not in the interval or _resultBuffer is empty,
    // so we need to move to the next interval.
    if (WorkingSet::INVALID_ID == resultID) {
        _nextInterval = nullptr;
        _nextIntervalStats = nullptr;
        _searchState = SearchState::Buffering;
        return PlanStage::NEED_TIME;
    }

    *toReturn = resultID;

    // This value is used by nextInterval() to determine the size of the next interval.
    ++_nextIntervalStats->numResultsReturned;

    return PlanStage::ADVANCED;
}

SortOptions NearStage::makeSortOptions() {
    if (feature_flags::gFeatureFlagExtendedAutoSpilling.isEnabled()) {
        return SortOptions{}
            .FileStats(&_sorterFileStats)
            // Spilling will handled externally by NearStage::spill method
            .MaxMemoryUsageBytes(std::numeric_limits<int64_t>::max())
            .TempDir(expCtx()->getTempDir());
    } else {
        return SortOptions{}.MaxMemoryUsageBytes(std::numeric_limits<int64_t>::max());
    }
}

void NearStage::updateSpillingStats() {

    auto additionalSpilledBytes = _sorterFileStats.bytesSpilledUncompressed() -
        _specificStats.spillingStats.getSpilledBytes();

    auto spilledDataStorageIncrease = _specificStats.spillingStats.updateSpillingStats(
        1 /*spills*/,
        additionalSpilledBytes,
        _resultBuffer.stats().spilledKeyValuePairs(),
        _sorterFileStats.bytesSpilled());

    geoNearCounters.incrementPerSpilling(1,
                                         additionalSpilledBytes,
                                         _resultBuffer.stats().spilledKeyValuePairs(),
                                         spilledDataStorageIncrease);
}

void NearStage::spill(uint64_t maxMemoryBytes) {
    if (_resultBuffer.stats().memUsage() <= maxMemoryBytes) {
        return;
    }
    _resultBuffer.forceSpill();
    updateSpillingStats();
}

bool NearStage::isEOF() const {
    return SearchState::Finished == _searchState;
}

std::unique_ptr<PlanStageStats> NearStage::getStats() {
    auto ret = std::make_unique<PlanStageStats>(_commonStats, _stageType);
    updateSpillingStats();
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
