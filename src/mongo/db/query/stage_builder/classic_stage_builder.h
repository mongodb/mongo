// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/stage_builder/stage_builder.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {

/**
 * Map from PlanStageKey to the QuerySolutionNode* which generated it.
 */
using PlanStageToQsnMap = absl::flat_hash_map<PlanStageKey, const QuerySolutionNode*>;

/**
 * A stage builder which builds an executable tree using classic PlanStages.
 */
class ClassicStageBuilder : public StageBuilder<std::unique_ptr<PlanStage>> {
public:
    using PlanType = std::unique_ptr<PlanStage>;

    ClassicStageBuilder(OperationContext* opCtx,
                        CollectionAcquisition collection,
                        const CanonicalQuery& cq,
                        const QuerySolution& solution,
                        WorkingSet* ws,
                        PlanStageToQsnMap* planStageQsnMap)
        : StageBuilder<PlanType>{opCtx, cq, solution},
          _collection(collection),
          _ws{ws},
          _planStageQsnMap(planStageQsnMap) {}

    PlanType build(const QuerySolutionNode* root) final;

private:
    CollectionAcquisition _collection;
    WorkingSet* _ws;

    boost::optional<size_t> _ftsKeyPrefixSize;

    // We don't own this, we populate it during the build phase.
    PlanStageToQsnMap* _planStageQsnMap;
};
}  // namespace mongo::stage_builder
