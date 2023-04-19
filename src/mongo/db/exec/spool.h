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


#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo {

/**
 * Spools the inputs received from the child in a buffer. This is an eager spool: initially, on each
 * call to doWork() it reads as much as it can from its child (propagating yields and NEED_TIME) and
 * caches it in a buffer. Once the input has been exhausted, calls to doWork() return the cached
 * results.
 *
 * Note that to reduce memory and disk usage, the spool only caches RecordIds. Callers must fetch
 * the corresponding documents as needed.
 */
class SpoolStage final : public PlanStage {
public:
    static const char* kStageType;

    SpoolStage(ExpressionContext* expCtx, WorkingSet* ws, std::unique_ptr<PlanStage> child);

    StageType stageType() const {
        return STAGE_SPOOL;
    }

    bool isEOF() final;

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const {
        return &_specificStats;
    }

protected:
    PlanStage::StageState doWork(WorkingSetID* id);

private:
    void spill();

    WorkingSet* _ws;

    SpoolStats _specificStats;

    // Next index to consume from the buffer. If < 0, the buffer is not yet fully populated from the
    // child.
    int _nextIndex = -1;

    // Buffer caching spooled results in-memory.
    std::vector<RecordId> _buffer;

    // Machinery for spilling to disk.
    MemoryUsageTracker _memTracker;
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<Sorter<RecordId, NullValue>::File> _file;

    // Iterators over the file that has been spilled to disk. These must be exhausted in addition to
    // '_buffer' when returning results.
    std::deque<std::shared_ptr<Sorter<RecordId, NullValue>::Iterator>> _spillFileIters;
};
}  //  namespace mongo
