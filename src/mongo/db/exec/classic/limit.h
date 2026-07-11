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
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This stage implements limit functionality.  It only returns 'limit' results before EOF.
 *
 * Sort has a baked-in limit, as it can optimize the sort if it has a limit.
 *
 * Preconditions: None.
 */
class LimitStage final : public PlanStage {
public:
    LimitStage(ExpressionContext* expCtx,
               long long limit,
               WorkingSet* ws,
               std::unique_ptr<PlanStage> child);
    ~LimitStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_LIMIT;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "LIMIT"sv;

private:
    // We only return this many results.
    long long _numToReturn;

    // Stats
    LimitStats _specificStats;
};

}  // namespace mongo
