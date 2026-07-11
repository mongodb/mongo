// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/recordid_deduplicator.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This stage outputs the union of its children. It optionally deduplicates on RecordId.
 *
 * Preconditions: Valid RecordId.
 */
class OrStage final : public PlanStage {
public:
    OrStage(ExpressionContext* expCtx, WorkingSet* ws, bool dedup, const MatchExpression* filter);

    void addChild(std::unique_ptr<PlanStage> child);

    void addChildren(Children childrenToAdd);

    bool isEOF() const final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_OR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    const SimpleMemoryUsageTracker& getMemoryTracker_forTest() {
        return _memoryTracker;
    }

    static constexpr std::string_view kStageType = "OR"sv;

private:
    // Not owned by us.
    WorkingSet* _ws;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // Which of _children are we calling work(...) on now?
    size_t _currentChild;

    // True if we dedup on RecordId, false otherwise.
    const bool _dedup;

    // Which RecordIds have we returned?
    RecordIdDeduplicator _recordIdDeduplicator;

    // Stats
    OrStats _specificStats;

    // Track memory used by this stage for deduplicating.
    SimpleMemoryUsageTracker _memoryTracker;

    DeduplicatorReporter _dedupReporter;
};

}  // namespace mongo
