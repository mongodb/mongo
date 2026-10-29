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
 * What a classic PlanStage was built from: the QuerySolutionNode that generated it, plus that
 * node's id captured by value. Note that the QuerySolutionNode pointer is not owned here and only
 * certain node types are guaranteed to be stable across the lifetime of the PlanStage tree.
 *
 * TODO SERVER-134683 Remove explain's reliance on stable (unowned) QuerySolutionNode raw pointers
 */
struct QsnMapping {
    const QuerySolutionNode* qsn = nullptr;
    PlanNodeId nodeId = 0;

    bool operator==(const QsnMapping&) const = default;
};

/**
 * Map from PlanStageKey to what generated it.
 */
using PlanStageToQsnMap = absl::flat_hash_map<PlanStageKey, QsnMapping>;

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
