// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Iterates over a collection using multiple underlying RecordCursors.
 *
 * This is a special stage which is not used automatically by queries. It is intended for special
 * commands that work with RecordCursors.
 */
class MultiIteratorStage final : public RequiresCollectionStage {
public:
    MultiIteratorStage(ExpressionContext* expCtx, WorkingSet* ws, CollectionAcquisition collection);

    void addIterator(std::unique_ptr<RecordCursor> it);

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() const final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() final;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    // Not used.
    StageType stageType() const final {
        return STAGE_MULTI_ITERATOR;
    }

    static constexpr std::string_view kStageType = "MULTI_ITERATOR"sv;

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    std::vector<std::unique_ptr<RecordCursor>> _iterators;

    // Not owned by us.
    WorkingSet* _ws;
};

}  // namespace mongo
