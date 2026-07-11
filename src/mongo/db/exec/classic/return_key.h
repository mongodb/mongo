// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {
/**
 * This stage returns the index key or keys, used for executing the query, for each document in
 * the result. If the query does not use an index to perform the read operation, the stage returns
 * empty documents.
 *
 * If 'sortKeyMetaFields' vector is specified while constructing this stage, each element in this
 * array will be treated as a field name which will be added to the output document and will
 * hold a sort key for the document in the result set, if the sort key exists. The elements in this
 * array are the values specified in the 'sortKey' meta-projection.
 */
class ReturnKeyStage final : public PlanStage {
public:
    static constexpr const char* kStageName = "RETURN_KEY";

    ReturnKeyStage(ExpressionContext* expCtx,
                   std::vector<FieldPath> sortKeyMetaFields,
                   WorkingSet* ws,
                   std::unique_ptr<PlanStage> child)
        : PlanStage(expCtx, std::move(child), kStageName),
          _ws(*ws),
          _sortKeyMetaFields(std::move(sortKeyMetaFields)) {}

    StageType stageType() const final {
        return STAGE_RETURN_KEY;
    }

    bool isEOF() const final {
        return child()->isEOF();
    }

    StageState doWork(WorkingSetID* out) final;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

private:
    void _extractIndexKey(WorkingSetMember* member);

    WorkingSet& _ws;
    ReturnKeyStats _specificStats;

    // The field names associated with any sortKey meta-projection(s). Empty if there is no sortKey
    // meta-projection.
    std::vector<FieldPath> _sortKeyMetaFields;
};
}  // namespace mongo
