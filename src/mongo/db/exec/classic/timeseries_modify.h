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
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/global_catalog/router_role_api/sharding_write_router.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

struct TimeseriesModifyParams {
    TimeseriesModifyParams(const DeleteStageParams* deleteParams)
        : isUpdate(false),
          isMulti(deleteParams->isMulti),
          fromMigrate(deleteParams->fromMigrate),
          isExplain(deleteParams->isExplain),
          returnOld(deleteParams->returnDeleted),
          stmtId(deleteParams->stmtId),
          canonicalQuery(deleteParams->canonicalQuery) {}

    TimeseriesModifyParams(const UpdateStageParams* updateParams)
        : isUpdate(true),
          isMulti(updateParams->request->isMulti()),
          fromMigrate(updateParams->request->source() == OperationSource::kFromMigrate),
          isExplain(updateParams->request->explain()),
          returnOld(updateParams->request->shouldReturnOldDocs()),
          returnNew(updateParams->request->shouldReturnNewDocs()),
          allowShardKeyUpdatesWithoutFullShardKeyInQuery(
              updateParams->request->getAllowShardKeyUpdatesWithoutFullShardKeyInQuery()),
          canonicalQuery(updateParams->canonicalQuery),
          isFromOplogApplication(updateParams->request->isFromOplogApplication()),
          updateDriver(updateParams->driver) {
        tassert(7314203,
                "timeseries updates should only have one stmtId",
                updateParams->request->getStmtIds().size() == 1);
        stmtId = updateParams->request->getStmtIds().front();
    }

    // Is this an update or a delete operation?
    bool isUpdate;

    // Is this a multi update/delete?
    bool isMulti;

    // Is this command part of a migrate operation that is essentially like a no-op when the
    // cluster is observed by an external client.
    bool fromMigrate;

    // Are we explaining a command rather than actually executing it?
    bool isExplain;

    // Should we return the old measurement?
    bool returnOld;

    // Should we return the new measurement?
    bool returnNew = false;

    // Should we allow shard key updates without the full shard key in the query?
    OptionalBool allowShardKeyUpdatesWithoutFullShardKeyInQuery;

    // The stmtId for this particular command.
    StmtId stmtId = kUninitializedStmtId;

    // The parsed query predicate for this command. Not owned here.
    CanonicalQuery* canonicalQuery;

    // True if this command was triggered by the application of an oplog entry.
    bool isFromOplogApplication = false;

    // Contains the logic for applying mods to documents. Only present for updates. Not owned. Must
    // outlive the TimeseriesModifyStage.
    UpdateDriver* updateDriver = nullptr;
};

/**
 * Unpacks time-series bucket documents and writes the modified documents.
 *
 * The stage processes one bucket at a time, unpacking all the measurements and writing the output
 * bucket in a single doWork() call.
 */
class TimeseriesModifyStage : public RequiresWritableCollectionStage {
public:
    static const char* kStageType;

    TimeseriesModifyStage(ExpressionContext* expCtx,
                          TimeseriesModifyParams&& params,
                          WorkingSet* ws,
                          std::unique_ptr<PlanStage> child,
                          CollectionAcquisition coll,
                          timeseries::BucketUnpacker bucketUnpacker,
                          std::unique_ptr<MatchExpression> residualPredicate,
                          std::unique_ptr<MatchExpression> originalPredicate = nullptr);
    ~TimeseriesModifyStage() override;

    StageType stageType() const override {
        return STAGE_TIMESERIES_MODIFY;
    }

    bool isEOF() const override;

    std::unique_ptr<PlanStageStats> getStats() override;

    const SpecificStats* getSpecificStats() const override {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id) override;

    bool containsDotsAndDollarsField() const {
        return _params.isUpdate && _params.updateDriver->containsDotsAndDollarsField();
    }

protected:
    void doSaveStateRequiresCollection() final {
        _preWriteFilter.saveState();
    }

    void doRestoreStateRequiresCollection() final;

    /**
     * Prepares returning the old or new measurement when requested so.
     */
    void _prepareToReturnMeasurement(WorkingSetID& out, BSONObj measurement);

    /**
     * Gets the user-level shard key paths.
     */
    const std::vector<std::unique_ptr<FieldRef>>& _getUserLevelShardKeyPaths(
        const ScopedCollectionDescription& collDesc);

    /**
     * Gets immutable paths when the request is user-initiated and the timeseries collection is
     * sharded and the request does not come from the router.
     */
    const std::vector<std::unique_ptr<FieldRef>>& _getImmutablePaths();

    // A user-initiated write is one which is not caused by oplog application and is not part of a
    // chunk migration.
    bool _isUserInitiatedUpdate;

    TimeseriesModifyParams _params;

    TimeseriesModifyStats _specificStats{};

    // Original, untranslated and complete predicate.
    std::unique_ptr<MatchExpression> _originalPredicate;

    // Temporary storage for _getImmutablePaths().
    std::vector<std::unique_ptr<FieldRef>> _immutablePaths;

    // Manages the updated measurements in a separate set of buckets through a side bucket catalog
    // when performing time-series updates,
    std::unique_ptr<timeseries::bucket_catalog::BucketCatalog> _sideBucketCatalog = nullptr;

    // BucketIds of newly inserted buckets for the updated measurements.
    std::set<timeseries::bucket_catalog::BucketId> _insertedBucketIds{};

private:
    bool _isMultiWrite() const {
        return _params.isMulti;
    }

    bool _isSingletonWrite() const {
        return !_isMultiWrite();
    }

    /**
     * Applies update and returns the updated measurements.
     */
    std::vector<BSONObj> _applyUpdate(const std::vector<BSONObj>& matchedMeasurements,
                                      std::vector<BSONObj>& unchangedMeasurements);

    /**
     * Writes the modifications to a bucket.
     *
     * Returns the tuple of (whether the write was successful, the stage state to propagate, the
     * matched/modified measurement to return if the write was successful).
     *
     * The current minTime of the bucket we are deleting measurements from/updating is passed down
     * so that we leave it unchanged. We do not want it to change because we use the minTime as the
     * key for emplacing/retrieving buckets from the set of archived buckets, and because the
     * minTime of a bucket is used as the shard key when sharding a time-series collection on time.
     */
    template <typename F>
    std::tuple<bool, PlanStage::StageState, boost::optional<BSONObj>> _writeToTimeseriesBuckets(
        ScopeGuard<F>& bucketFreer,
        WorkingSetID bucketWsmId,
        std::vector<BSONObj>&& unchangedMeasurements,
        std::vector<BSONObj>&& matchedMeasurements,
        Date_t currentMinTime,
        bool bucketFromMigrate);

    /**
     * Helper to set up state to retry 'bucketId' after yielding and establishing a new storage
     * snapshot.
     */
    void _retryBucket(WorkingSetID bucketId);

    template <typename F>
    std::pair<boost::optional<PlanStage::StageState>, bool> _checkIfWritingToOrphanedBucket(
        ScopeGuard<F>& bucketFreer, WorkingSetID id);

    /**
     * Gets the next bucket to process.
     */
    PlanStage::StageState _getNextBucket(WorkingSetID& id);

    void _checkRestrictionsOnUpdatingShardKeyAreNotViolated(
        const ScopedCollectionDescription& collDesc, const FieldRefSet& shardKeyPaths);

    void _checkUpdateChangesExistingShardKey(const BSONObj& newBucket,
                                             const BSONObj& oldBucket,
                                             const BSONObj& newMeasurement,
                                             const BSONObj& oldMeasurement);

    void _checkUpdateChangesReshardingKey(const ShardingWriteRouter& shardingWriteRouter,
                                          const BSONObj& newBucket,
                                          const BSONObj& oldBucket,
                                          const BSONObj& newMeasurement,
                                          const BSONObj& oldMeasurementt);

    void _checkUpdateChangesShardKeyFields(const BSONObj& newBucket,
                                           const BSONObj& oldBucket,
                                           const BSONObj& newMeasurement,
                                           const BSONObj& oldMeasurement);

    WorkingSet* _ws;

    //
    // Main execution machinery data structures.
    //

    timeseries::BucketUnpacker _bucketUnpacker;

    // Determines the measurements to modify in this bucket, and by inverse, those to keep
    // unmodified. This predicate can be null if we have a meta-only or empty predicate on singleton
    // deletes or updates.
    std::unique_ptr<MatchExpression> _residualPredicate;

    /**
     * This member is used to check whether the write should be performed, and if so, any other
     * behavior that should be done as part of the write (e.g. skipping it because it affects an
     * orphan document). A yield cannot happen between the check and the write, so the checks are
     * embedded in the stage.
     *
     * It's refreshed after yielding and reacquiring the locks.
     */
    write_stage_common::PreWriteFilter _preWriteFilter;

    // A pending retry to get to after a NEED_YIELD propagation and a new storage snapshot is
    // established. This can be set when a write fails or when a fetch fails.
    WorkingSetID _retryBucketId = WorkingSet::INVALID_ID;
};
}  //  namespace mongo
