/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/stage_builder/stage_builder.h"

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
                        VariantCollectionPtrOrAcquisition collection,
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
    VariantCollectionPtrOrAcquisition _collection;
    WorkingSet* _ws;

    boost::optional<size_t> _ftsKeyPrefixSize;

    // We don't own this, we populate it during the build phase.
    PlanStageToQsnMap* _planStageQsnMap;
};
}  // namespace mongo::stage_builder
