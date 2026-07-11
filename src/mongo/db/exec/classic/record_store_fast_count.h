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
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Implements "fast count" by asking the underlying RecordStore for its number of records, applying
 * the skip and limit it necessary. The result is stored in '_specificStats'. Only used to answer
 * count commands when both the query and hint are empty.
 */
class RecordStoreFastCountStage final : public RequiresCollectionStage {
public:
    static constexpr std::string_view kStageType = "RECORD_STORE_FAST_COUNT"sv;

    RecordStoreFastCountStage(ExpressionContext* expCtx,
                              CollectionAcquisition collection,
                              long long skip,
                              long long limit);

    bool isEOF() const override {
        return _commonStats.isEOF;
    }

    StageState doWork(WorkingSetID* out) override;

    StageType stageType() const override {
        return StageType::STAGE_RECORD_STORE_FAST_COUNT;
    }

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const override {
        return &_specificStats;
    }

protected:
    void doSaveStateRequiresCollection() override {}

    void doRestoreStateRequiresCollection() override {}

private:
    long long _skip = 0;
    long long _limit = 0;

    CountStats _specificStats;
};

}  // namespace mongo
