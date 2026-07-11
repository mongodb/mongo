// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

namespace mongo::stage_builder {
/**
 * The StageBuilder converts a QuerySolution tree to an executable tree of PlanStage(s), with the
 * specific type defined by the 'PlanType' parameter.
 */
template <typename PlanType>
class StageBuilder {
public:
    StageBuilder(OperationContext* opCtx, const CanonicalQuery& cq, const QuerySolution& solution)
        : _opCtx(opCtx), _cq(cq), _solution(solution) {}

    virtual ~StageBuilder() = default;

    /**
     * Given a root node of a QuerySolution tree, builds and returns a corresponding executable
     * tree of PlanStages.
     */
    virtual PlanType build(const QuerySolutionNode* root) = 0;

protected:
    OperationContext* _opCtx;
    const CanonicalQuery& _cq;
    const QuerySolution& _solution;
};
}  // namespace mongo::stage_builder
