// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/s/resharding/sharding_write_router.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

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
    static constexpr std::string_view kStageType = "TS_MODIFY"sv;

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

    void _checkUpdateChangesReshardingKey(const BSONObj& newBucket,
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
