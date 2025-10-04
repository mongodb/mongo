/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/timeseries_modify.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/write_ops/update_request.h"
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
