// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/util/modules.h"

namespace mongo::stage_builder {

boost::optional<PlanStageReqs> makeExtractFieldPathsPlanStageReqs(
    StageBuilderState& state,
    const std::vector<const Expression*>& expressions,
    const PlanStageSlots& childStageOutputs);

std::pair<SbStage, PlanStageSlots> buildExtractFieldPaths(SbStage stage,
                                                          StageBuilderState& state,
                                                          const PlanStageSlots& childStageOutputs,
                                                          PlanStageReqs& extractFieldPathsReqs,
                                                          PlanNodeId nodeId);
}  // namespace mongo::stage_builder
