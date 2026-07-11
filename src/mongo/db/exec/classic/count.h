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
 * Stage used by the count command. This stage sits at the root of a plan tree and counts the number
 * of results returned by its child stage.
 *
 * This should not be confused with the CountScan stage. CountScan is a special index access stage
 * which can optimize index access for count operations in some cases. On the other hand, *every*
 * count op has a CountStage at its root.
 *
 * Only returns NEED_TIME until hitting EOF. The count result can be obtained by examining the
 * specific stats.
 */
class CountStage final : public PlanStage {
public:
    CountStage(ExpressionContext* expCtx,
               long long limit,
               long long skip,
               WorkingSet* ws,
               PlanStage* child);

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_COUNT;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "COUNT"sv;

    long long getLimit() const {
        return _limit;
    }

    long long getSkip() const {
        return _skip;
    }

private:
    // An integer limiting the number of documents to count. 0 means no limit.
    long long _limit;

    // An integer indicating to not include the first n documents in the count. 0 means no skip.
    long long _skip;

    // The number of documents that we still need to skip.
    long long _leftToSkip;

    // The working set used to pass intermediate results between stages. Not owned
    // by us.
    WorkingSet* _ws;

    CountStats _specificStats;
};

}  // namespace mongo
