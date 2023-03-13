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

#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/requires_collection_stage.h"

namespace mongo {

/**
 * Unpacks time-series bucket documents and writes the modified documents.
 *
 * The stage processes one measurement at a time, but only performs a write after each bucket is
 * exhausted.
 */
class TimeseriesModifyStage final : public RequiresMutableCollectionStage {
public:
    static const char* kStageType;

    TimeseriesModifyStage(ExpressionContext* expCtx,
                          WorkingSet* ws,
                          std::unique_ptr<PlanStage> child,
                          const CollectionPtr& coll,
                          BucketUnpacker bucketUnpacker,
                          std::unique_ptr<MatchExpression> residualPredicate);

    StageType stageType() const {
        return STAGE_TIMESERIES_MODIFY;
    }

    bool isEOF() final;

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id);

protected:
    void doSaveStateRequiresCollection() final {}

    void doRestoreStateRequiresCollection() final {}

private:
    /**
     * Writes the modifications to a bucket when the end of the bucket is detected.
     */
    void _writeToTimeseriesBuckets();

    /**
     * Fetches the document for the bucket pointed to by this WSM.
     */
    PlanStage::StageState _fetchBucket(WorkingSetID id);

    /**
     * Gets the next bucket to process.
     */
    PlanStage::StageState _getNextBucket(WorkingSetID& id);

    WorkingSet* _ws;

    //
    // Main execution machinery data structures.
    //

    BucketUnpacker _bucketUnpacker;

    // Determines the measurements to delete from this bucket, and by inverse, those to keep
    // unmodified.
    std::unique_ptr<MatchExpression> _residualPredicate;

    // The RecordId (also "_id" for the clustered collection) value of the current bucket.
    RecordId _currentBucketRid = RecordId{};

    // The WS id of the next bucket to unpack. This is populated in cases where we successfully read
    // the RecordId of the next bucket, but receive NEED_YIELD when attempting to fetch the full
    // document.
    WorkingSetID _retryFetchBucketId = WorkingSet::INVALID_ID;

    std::vector<BSONObj> _unchangedMeasurements;
    std::vector<BSONObj> _deletedMeasurements;

    TimeseriesModifyStats _specificStats{};
};
}  //  namespace mongo
