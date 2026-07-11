// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/util/modules.h"

#include <deque>
#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

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
    static constexpr std::string_view kStageType = "SPOOL"sv;

    SpoolStage(ExpressionContext* expCtx, WorkingSet* ws, std::unique_ptr<PlanStage> child);

    StageType stageType() const override {
        return STAGE_SPOOL;
    }

    bool isEOF() const final;

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const override {
        return &_specificStats;
    }

    void doForceSpill() final {
        spill();
    }

    const MemoryUsageTracker* getMemoryUsageTracker_forTest() {
        return &_memTracker;
    }

protected:
    PlanStage::StageState doWork(WorkingSetID* id) override;

private:
    void spill();

    WorkingSet* _ws;

    SpoolStats _specificStats;

    // If false, the buffer is not yet fully populated from the child.
    bool allInputConsumed = false;
    // Last index that was consumed from buffer.
    int _nextIndex = -1;

    // Buffer caching spooled results in-memory.
    std::vector<RecordId> _buffer;

    // Machinery for spilling to disk.
    MemoryUsageTracker _memTracker;
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<sorter::File> _file;

    // Iterators over the file that has been spilled to disk. These must be exhausted in addition to
    // '_buffer' when returning results.
    std::deque<std::shared_ptr<Sorter<RecordId, NullValue>::Iterator>> _spillFileIters;
};
}  //  namespace mongo
