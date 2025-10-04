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

#include "mongo/db/exec/classic/or.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

#include <iterator>
#include <memory>
#include <utility>
#include <vector>


namespace mongo {

using std::unique_ptr;

// static
const char* OrStage::kStageType = "OR";

OrStage::OrStage(ExpressionContext* expCtx,
                 WorkingSet* ws,
                 bool dedup,
                 const MatchExpression* filter)
    : PlanStage(kStageType, expCtx),
      _ws(ws),
      _filter(filter),
      _currentChild(0),
      _dedup(dedup),
      _recordIdDeduplicator(expCtx),
      _memoryTracker(OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(*expCtx)) {
}

void OrStage::addChild(std::unique_ptr<PlanStage> child) {
    _children.emplace_back(std::move(child));
}

void OrStage::addChildren(Children childrenToAdd) {
    _children.insert(_children.end(),
                     std::make_move_iterator(childrenToAdd.begin()),
                     std::make_move_iterator(childrenToAdd.end()));
}

bool OrStage::isEOF() const {
    return _currentChild >= _children.size();
}

PlanStage::StageState OrStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState childStatus = _children[_currentChild]->work(&id);

    if (PlanStage::ADVANCED == childStatus) {
        WorkingSetMember* member = _ws->get(id);

        // If we're deduping (and there's something to dedup by)
        if (_dedup && member->hasRecordId()) {
            uint64_t dedupBytesPrev = _recordIdDeduplicator.getApproximateSize();

            ++_specificStats.dupsTested;

            // ...and we've seen the RecordId before
            if (!_recordIdDeduplicator.insert(member->recordId)) {
                // ...drop it.
                ++_specificStats.dupsDropped;
                _ws->free(id);
                return PlanStage::NEED_TIME;
            } else {
                uint64_t dedupBytes = _recordIdDeduplicator.getApproximateSize();
                _memoryTracker.add(dedupBytes - dedupBytesPrev);
                _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();
            }
        }

        if (Filter::passes(member, _filter)) {
            // Match!  return it.
            *out = id;
            return PlanStage::ADVANCED;
        } else {
            // Does not match, try again.
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }
    } else if (PlanStage::IS_EOF == childStatus) {
        // Done with _currentChild, move to the next one.
        ++_currentChild;

        // Maybe we're out of children.
        if (isEOF()) {
            return PlanStage::IS_EOF;
        } else {
            return PlanStage::NEED_TIME;
        }
    } else if (PlanStage::NEED_YIELD == childStatus) {
        *out = id;
    }

    // NEED_TIME, ERROR, NEED_YIELD, pass them up.
    return childStatus;
}

unique_ptr<PlanStageStats> OrStage::getStats() {
    _commonStats.isEOF = isEOF();

    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (_filter) {
        _commonStats.filter = _filter->serialize();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_OR);
    ret->specific = std::make_unique<OrStats>(_specificStats);
    for (size_t i = 0; i < _children.size(); ++i) {
        ret->children.emplace_back(_children[i]->getStats());
    }

    return ret;
}

const SpecificStats* OrStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
