// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class CollatorInterface;
class Collection;
class CollectionPtr;
class WorkingSetMember;

/**
 * Passes results from the child through after adding the sort key for each result as
 * WorkingSetMember metadata.
 */
class SortKeyGeneratorStage final : public PlanStage {
public:
    SortKeyGeneratorStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          std::unique_ptr<PlanStage> child,
                          WorkingSet* ws,
                          const BSONObj& sortSpecObj);

    bool isEOF() const final;

    StageType stageType() const final {
        return STAGE_SORT_KEY_GENERATOR;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "SORT_KEY_GENERATOR"sv;

protected:
    StageState doWork(WorkingSetID* out) final;

private:
    WorkingSet* const _ws;

    SortKeyGenerator _sortKeyGen;
};

}  // namespace mongo
