// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/spool.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_file_name.h"
#include "mongo/db/sorter/sorter_template_defs.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <utility>

namespace {
// Helper to allocate a new working set member to hold the RecordId, set the output parameter, and
// return ADVANCED.
mongo::PlanStage::StageState allocateResultAndAdvance(mongo::WorkingSet* ws,
                                                      mongo::WorkingSetID* out,
                                                      mongo::RecordId&& recordId) {
    *out = ws->allocate();
    auto member = ws->get(*out);
    member->recordId = std::move(recordId);
    // Only store the record id, not any index information or full objects. This is to
    // reduce memory and disk usage - it is the responsibility of our caller to fetch the records.
    ws->transitionToRecordIdAndIdx(*out);
    return mongo::PlanStage::ADVANCED;
}
}  // namespace

namespace mongo {


SpoolStage::SpoolStage(ExpressionContext* expCtx, WorkingSet* ws, std::unique_ptr<PlanStage> child)
    : PlanStage(expCtx, std::move(child), kStageType),
      _ws(ws),
      _memTracker(OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
          *expCtx,
          expCtx->getAllowDiskUse() && !expCtx->getInRouter(),
          loadMemoryLimit(StageMemoryLimit::QueryMaxSpoolMemoryUsageBytes))) {
    _specificStats.maxMemoryUsageBytes =
        _memTracker.maxAllowedMemoryUsageBytes(expCtx->getOperationContext());
    _specificStats.maxDiskUsageBytes = internalQueryMaxSpoolDiskUsageBytes.load();
}

bool SpoolStage::isEOF() const {
    return allInputConsumed && _spillFileIters.empty() && _buffer.empty();
}

std::unique_ptr<PlanStageStats> SpoolStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<SpoolStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

void SpoolStage::spill() {
    _memTracker.assertCanSpill("spool");
    uassert(7443700,
            "Exceeded disk use limit for spool",
            _specificStats.spillingStats.getSpilledDataStorageSize() <
                _specificStats.maxDiskUsageBytes);

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _file = std::make_shared<sorter::File>(sorter::nextFileName(expCtx()->getTempDir()),
                                               _spillStats.get());
    }

    auto opts = SortOptions();

    sorter::FileBasedStorage<RecordId, NullValue> sorterStorage(_file,
                                                                /*dbName=*/boost::none,
                                                                sorter::kLatestChecksumVersion);
    std::unique_ptr<SortedStorageWriter<RecordId, NullValue>> writer =
        sorterStorage.makeWriter(opts, /*settings=*/{});
    // Do not spill the records that have been already consumed.
    for (size_t i = _nextIndex + 1; i < _buffer.size(); ++i) {
        writer->addAlreadySorted(_buffer[i], NullValue());
    }
    _spillFileIters.emplace_back(writer->done());

    _specificStats.spillingStats.updateSpillingStats(1 /* spills */,
                                                     _memTracker.inUseTrackedMemoryBytes(),
                                                     _buffer.size() - (_nextIndex + 1),
                                                     _spillStats->bytesSpilled());
    std::vector<RecordId>().swap(_buffer);
    _nextIndex = -1;
    _memTracker.resetCurrent();
}

PlanStage::StageState SpoolStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        _specificStats.peakTrackedMemBytes = _memTracker.peakTrackedMemoryBytes();
        _memTracker.resetCurrent();
        return PlanStage::IS_EOF;
    }

    if (!allInputConsumed) {
        // We have not yet received EOF from our child yet. Eagerly consume and cache results as
        // long as the child keeps advancing (we'll propagate yields and NEED_TIME).
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = child()->work(&id);

        while (status == PlanStage::ADVANCED) {
            // The child has returned another result, put it in our cache.
            auto member = _ws->get(id);
            tassert(
                7443500, "WSM passed to spool stage must have a RecordId", member->hasRecordId());

            auto memUsage = member->recordId.memUsage();
            _specificStats.totalDataSizeBytes += memUsage;
            _memTracker.add(memUsage);

            _buffer.emplace_back(std::move(member->recordId));

            if (!_memTracker.withinMemoryLimit(opCtx())) {
                spill();
            }

            // We've cached the RecordId, so go ahead and free the object in the working set.
            _ws->free(id);

            // Ask the child for another record.
            status = child()->work(&id);
        }

        if (status != PlanStage::IS_EOF) {
            *out = id;
            return status;
        }

        // The child has returned all of its results. Fall through and begin consuming the results
        // from our buffer.
        allInputConsumed = true;
    }

    // First, return results from any spills we may have.
    while (!_spillFileIters.empty()) {
        if (_spillFileIters.front()->more()) {
            auto [recordId, _] = _spillFileIters.front()->next();
            return allocateResultAndAdvance(_ws, out, std::move(recordId));
        }

        _spillFileIters.pop_front();
    }

    // Increment to the next element in our buffer. Note that we increment the index *first* so that
    // we will return EOF in a call to doWork() before isEOF() returns true.
    if (++_nextIndex == static_cast<int>(_buffer.size())) {
        std::vector<RecordId>().swap(_buffer);
        _nextIndex = 0;
        _specificStats.peakTrackedMemBytes = _memTracker.peakTrackedMemoryBytes();
        _memTracker.resetCurrent();
        return PlanStage::IS_EOF;
    }

    tassert(9918000,
            "Unexpected _nextIndex value. It points outside the buffer.",
            _nextIndex < static_cast<int>(_buffer.size()));

    return allocateResultAndAdvance(_ws, out, std::move(_buffer[_nextIndex]));
}
}  // namespace mongo
