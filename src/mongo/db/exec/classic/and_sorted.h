// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <queue>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Reads from N children, each of which must have a valid RecordId. Assumes each child produces
 * RecordIds in sorted order. Outputs the intersection of the RecordIds outputted by the children.
 *
 * Preconditions: Valid RecordId. More than one child.
 */
class AndSortedStage final : public PlanStage {
public:
    AndSortedStage(ExpressionContext* expCtx, WorkingSet* ws);

    void addChild(std::unique_ptr<PlanStage> child);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_AND_SORTED;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "AND_SORTED"sv;

private:
    // Find a node to AND against.
    PlanStage::StageState getTargetRecordId(WorkingSetID* out);

    // Move a child which hasn't advanced to the target node forward.
    // Returns the target node in 'out' if all children successfully advance to it.
    PlanStage::StageState moveTowardTargetRecordId(WorkingSetID* out);

    // Not owned by us.
    WorkingSet* _ws;

    // The current node we're AND-ing against.
    size_t _targetNode;
    RecordId _targetRecordId;
    WorkingSetID _targetId;

    // Nodes we're moving forward until they hit the element we're AND-ing.
    // Everything in here has not advanced to _targetRecordId yet.
    // These are indices into _children.
    std::queue<size_t> _workingTowardRep;

    // If any child hits EOF or if we have any errors, we're EOF.
    bool _isEOF;

    // Stats
    AndSortedStats _specificStats;
};

}  // namespace mongo
