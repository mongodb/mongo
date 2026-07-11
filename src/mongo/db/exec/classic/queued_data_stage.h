// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <queue>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class RecordId;

/**
 * QueuedDataStage is a data-producing stage.  Unlike the other two leaf stages (CollectionScan
 * and IndexScan) QueuedDataStage does not require any underlying storage layer.
 *
 * A QueuedDataStage is "programmed" by pushing return values from work() onto its internal
 * queue.  Calls to QueuedDataStage::work() pop values off that queue and return them in FIFO
 * order, annotating the working set with data when appropriate.
 *
 * TODO SERVER-112968: Remove uses of this stage outside of the 'query' module.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] QueuedDataStage final : public PlanStage {
public:
    QueuedDataStage(ExpressionContext* expCtx, WorkingSet* ws);

    StageState doWork(WorkingSetID* out) final;

    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_QUEUED_DATA;
    }

    //
    // Exec stats
    //

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    /**
     * Add a result to the back of the queue.
     *
     * The caller is responsible for allocating 'id' and filling out the WSM keyed by 'id'
     * appropriately.
     *
     * The QueuedDataStage takes ownership of 'id', so the caller should not call WorkingSet::free()
     * on it.
     */
    void pushBack(const WorkingSetID& id);

    static constexpr std::string_view kStageType = "QUEUED_DATA"sv;

private:
    // The data we return.
    std::queue<WorkingSetID> _members;

    // Stats
    MockStats _specificStats;
};

}  // namespace mongo
