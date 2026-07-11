// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/timeseries_modify.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Execution stage for timeseries update requests with {upsert:true}. This is a specialized
 * TimeseriesModifyStage which, in the event that no documents match the update request's query,
 * generates and inserts a new document into the collection. All logic related to the insertion
 * phase is implemented by this class.
 */
class TimeseriesUpsertStage final : public TimeseriesModifyStage {
public:
    TimeseriesUpsertStage(ExpressionContext* expCtx,
                          TimeseriesModifyParams&& params,
                          WorkingSet* ws,
                          std::unique_ptr<PlanStage> child,
                          CollectionAcquisition coll,
                          timeseries::BucketUnpacker bucketUnpacker,
                          std::unique_ptr<MatchExpression> residualPredicate,
                          std::unique_ptr<MatchExpression> originalPredicate,
                          const UpdateRequest& request);

    bool isEOF() const final;
    PlanStage::StageState doWork(WorkingSetID* id) final;

private:
    BSONObj _produceNewDocumentForInsert();
    void _performInsert(BSONObj newDocument);

    // The original update request.
    const UpdateRequest& _request;
};
}  //  namespace mongo
