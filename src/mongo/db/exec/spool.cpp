/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/spool.h"

#include "mongo/platform/atomic_word.h"

namespace {
/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static mongo::AtomicWord<unsigned> spoolFileCounter;
    return "ext-spool." + std::to_string(spoolFileCounter.fetchAndAdd(1));
}

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

const char* SpoolStage::kStageType = "SPOOL";

SpoolStage::SpoolStage(ExpressionContext* expCtx, WorkingSet* ws, std::unique_ptr<PlanStage> child)
    : PlanStage(expCtx, std::move(child), kStageType),
      _ws(ws),
      _memTracker(expCtx->allowDiskUse, internalQueryMaxSpoolMemoryUsageBytes.load()) {

    _specificStats.maxMemoryUsageBytes = _memTracker._maxAllowedMemoryUsageBytes;
    _specificStats.maxDiskUsageBytes = internalQueryMaxSpoolDiskUsageBytes.load();
}

bool SpoolStage::isEOF() {
    return _spillFileIters.empty() && _nextIndex == static_cast<int>(_buffer.size());
}

std::unique_ptr<PlanStageStats> SpoolStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<SpoolStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

void SpoolStage::spill() {
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for spool, but didn't allow external sort. Set "
            "allowDiskUseByDefault:true to opt in.",
            _memTracker._allowDiskUse);
    uassert(7443700,
            "Exceeded disk use limit for spool",
            _specificStats.spilledDataStorageSize < _specificStats.maxDiskUsageBytes);

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _file = std::make_shared<Sorter<RecordId, NullValue>::File>(
            expCtx()->tempDir + "/" + nextFileName(), _spillStats.get());
    }

    SortedFileWriter<RecordId, NullValue> writer(SortOptions().TempDir(expCtx()->tempDir), _file);
    for (size_t i = 0; i < _buffer.size(); ++i) {
        writer.addAlreadySorted(std::move(_buffer[i]), NullValue());
    }
    _spillFileIters.emplace_back(writer.done());
    _buffer.clear();

    _memTracker.resetCurrent();
    ++_specificStats.spills;
    _specificStats.spilledDataStorageSize = _spillStats->bytesSpilled();
}

PlanStage::StageState SpoolStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (_nextIndex < 0) {
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
            _memTracker.update(memUsage);

            _buffer.emplace_back(std::move(member->recordId));

            if (!_memTracker.withinMemoryLimit()) {
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
        return PlanStage::IS_EOF;
    }

    return allocateResultAndAdvance(_ws, out, std::move(_buffer[_nextIndex]));
}
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
