// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/eof_node_type.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This stage just returns EOF immediately.
 */
class EOFStage final : public PlanStage {
public:
    EOFStage(ExpressionContext* expCtx, eof_node::EOFType type);

    ~EOFStage() override;

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;


    StageType stageType() const final {
        return STAGE_EOF;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "EOF"sv;

private:
    EofStats _specificStats;
};

}  // namespace mongo
