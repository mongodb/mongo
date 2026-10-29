// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * This PlanStage is the analog of DocumentSourceInternalUnpackBucket, but in the PlanStage layer.
 * It fetches a bucket from it's child as an owned BSONObj and uses the BucketUnpacker to
 * materialize time-series measurements until the time-series bucket collection is exhausted.
 */
class UnpackTimeseriesBucket final : public PlanStage {
public:
    static constexpr std::string_view kStageType = "UNPACK_BUCKET"sv;

    UnpackTimeseriesBucket(ExpressionContext* expCtx,
                           WorkingSet* ws,
                           std::unique_ptr<PlanStage> child,
                           timeseries::BucketUnpacker bucketUnpacker);

    StageType stageType() const final {
        return STAGE_UNPACK_SAMPLED_TS_BUCKET;
    }

    bool isEOF() const final {
        return !_bucketUnpacker.hasNext() && child()->isEOF();
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id) override;

private:
    WorkingSet& _ws;
    timeseries::BucketUnpacker _bucketUnpacker;
    UnpackTimeseriesBucketStats _specificStats;
};
}  //  namespace mongo
