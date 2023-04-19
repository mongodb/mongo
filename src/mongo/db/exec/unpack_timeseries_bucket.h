/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"

namespace mongo {
/**
 * This PlanStage is the analog of DocumentSourceInternalUnpackBucket, but in the PlanStage layer.
 * It fetches a bucket from it's child as an owned BSONObj and uses the BucketUnpacker to
 * materialize time-series measurements until the time-series bucket collection is exhausted.
 */
class UnpackTimeseriesBucket final : public PlanStage {
public:
    static const char* kStageType;

    UnpackTimeseriesBucket(ExpressionContext* expCtx,
                           WorkingSet* ws,
                           std::unique_ptr<PlanStage> child,
                           BucketUnpacker bucketUnpacker);

    StageType stageType() const final {
        return STAGE_UNPACK_TIMESERIES_BUCKET;
    }

    bool isEOF() final {
        return !_bucketUnpacker.hasNext() && child()->isEOF();
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id);

private:
    WorkingSet& _ws;
    BucketUnpacker _bucketUnpacker;
    UnpackTimeseriesBucketStats _specificStats;
};
}  //  namespace mongo
