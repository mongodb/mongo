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
 * This stage implements skip functionality.  It drops the first 'toSkip' results from its child
 * then returns the rest verbatim.
 *
 * Preconditions: None.
 */
class SkipStage final : public PlanStage {
public:
    SkipStage(ExpressionContext* expCtx,
              long long toSkip,
              WorkingSet* ws,
              std::unique_ptr<PlanStage> child);
    ~SkipStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SKIP;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "SKIP"sv;

private:
    WorkingSet* _ws;

    // The number of results left to skip. This number is decremented during query execution as we
    // successfully skip a document.
    long long _leftToSkip;

    // Represents the number of results to skip. Unlike '_leftToSkip', this remains constant and
    // is used when gathering statistics in explain.
    const long long _skipAmount;

    // Stats
    SkipStats _specificStats;
};

}  // namespace mongo
