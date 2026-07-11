// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/or.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/stats/counters.h"

#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using std::unique_ptr;


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
      _memoryTracker(OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(
          *expCtx, loadMemoryLimit(StageMemoryLimit::OrStageMaxMemoryBytes))),
      _dedupReporter(OperationMemoryUsageTracker::createDeduplicatorReporter(
          [](int64_t deduplicatedBytes, int64_t deduplicatedRecords) {
              orCounters.incrementPerDeduplication(deduplicatedBytes, deduplicatedRecords);
          },
          internalQueryMaxWriteToServerStatusMemoryUsageBytes.loadRelaxed())) {}

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
                _dedupReporter.add(dedupBytes - dedupBytesPrev);
                _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();
                uassert(11130302,
                        "Exceeded memory limit in record id deduplicator for OR stage",
                        _memoryTracker.withinMemoryLimit());
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
