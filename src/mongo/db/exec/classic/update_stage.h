/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/global_catalog/router_role_api/sharding_write_router.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/stdx/unordered_set.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

class OpDebug;
struct PlanSummaryStats;

struct UpdateStageParams {
    using DocumentCounter = std::function<size_t(const BSONObj&)>;

    UpdateStageParams(const UpdateRequest* r,
                      UpdateDriver* d,
                      OpDebug* o,
                      DocumentCounter&& documentCounter = nullptr)
        : request(r),
          driver(d),
          opDebug(o),
          canonicalQuery(nullptr),
          numStatsForDoc(std::move(documentCounter)) {}

    // Contains update parameters like whether it's a multi update or an upsert. Not owned.
    // Must outlive the UpdateStage.
    const UpdateRequest* request;

    // Contains the logic for applying mods to documents. Not owned. Must outlive
    // the UpdateStage.
    UpdateDriver* driver;

    // Needed to pass to Collection::updateDocument(...).
    OpDebug* opDebug;

    // Not owned here.
    CanonicalQuery* canonicalQuery;

    // Determines how the update stats should be incremented. Will be incremented by 1 if the
    // function is empty.
    DocumentCounter numStatsForDoc;

private:
    // Default constructor not allowed.
    UpdateStageParams();
};

class ShardingChecksForUpdate {
public:
    ShardingChecksForUpdate(const CollectionAcquisition& collAcq,
                            OptionalBool allowShardKeyUpdatesWithoutFullKey,
                            bool isMulti,
                            CanonicalQuery* cq)
        : _collAcq(collAcq),
          _allowShardKeyUpdatesWithoutFullShardKeyInQuery(allowShardKeyUpdatesWithoutFullKey),
          _isMulti(isMulti),
          _canonicalQuery(cq) {};

    /**
     * Performs checks on whether the existing or new shard key fields would change the owning
     * shard, including whether the owning shard under the current key pattern would change as a
     * result of the update, or if the destined recipient under the new shard key pattern from
     * resharding would change as a result of the update.
     *
     * Throws if the updated document does not have all of the shard key fields or no longer belongs
     * to this shard.
     *
     * Accepting a 'newObjCopy' parameter is a performance enhancement for updates which weren't
     * performed in-place to avoid rendering a full copy of the updated document multiple times.
     */
    void checkUpdateChangesShardKeyFields(OperationContext* opCtx,
                                          const mutablebson::Document& newDoc,
                                          const boost::optional<BSONObj>& newObjCopy,
                                          const Snapshotted<BSONObj>& oldObj);

private:
    /**
     * Checks that the updated doc has all required shard key fields and throws if it does not.
     *
     * Also checks if the updated doc still belongs to this node and throws if it does not. If the
     * doc no longer belongs to this shard, this means that one or more shard key field values have
     * been updated to a value belonging to a chunk that is not owned by this shard. We cannot apply
     * this update atomically.
     */
    void checkUpdateChangesExistingShardKey(OperationContext* opCtx,
                                            const mutablebson::Document& newDoc,
                                            const BSONObj& newObj,
                                            const Snapshotted<BSONObj>& oldObj);

    void checkUpdateChangesReshardingKey(OperationContext* opCtx,
                                         const ShardingWriteRouter& shardingWriteRouter,
                                         const BSONObj& newObj,
                                         const Snapshotted<BSONObj>& oldObj);

    void _checkRestrictionsOnUpdatingShardKeyAreNotViolated(
        OperationContext* opCtx,
        const ScopedCollectionDescription& collDesc,
        const FieldRefSet& shardKeyPaths);


    const CollectionAcquisition& _collAcq;
    const OptionalBool _allowShardKeyUpdatesWithoutFullShardKeyInQuery;
    const bool _isMulti;
    const CanonicalQuery* _canonicalQuery; /* can be nullptr */
};

/**
 * Execution stage responsible for updates to documents. If the prior or newly-updated version of
 * the document was requested to be returned, then ADVANCED is returned after updating a document.
 * Otherwise, NEED_TIME is returned after updating a document if further updates are pending,
 * and IS_EOF is returned if no documents were found or all updates have been performed.
 *
 * Callers of doWork() must be holding a write lock.
 */
class UpdateStage : public RequiresWritableCollectionStage {
    UpdateStage(const UpdateStage&) = delete;
    UpdateStage& operator=(const UpdateStage&) = delete;

public:
    static constexpr StringData kStageType = "UPDATE"_sd;

    UpdateStage(ExpressionContext* expCtx,
                const UpdateStageParams& params,
                WorkingSet* ws,
                CollectionAcquisition collection,
                PlanStage* child);

    bool isEOF() const override;
    StageState doWork(WorkingSetID* out) override;

    StageType stageType() const final {
        return STAGE_UPDATE;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    bool containsDotsAndDollarsField() const {
        return _params.driver->containsDotsAndDollarsField();
    }

protected:
    UpdateStage(ExpressionContext* expCtx,
                const UpdateStageParams& params,
                WorkingSet* ws,
                CollectionAcquisition collection);

    void doSaveStateRequiresCollection() final {
        _preWriteFilter.saveState();
    }

    void doRestoreStateRequiresCollection() final;

    void _checkRestrictionsOnUpdatingShardKeyAreNotViolated(
        const ScopedCollectionDescription& collDesc, const FieldRefSet& shardKeyPaths);

    UpdateStageParams _params;

    // Not owned by us.
    WorkingSet* _ws;

    // Stats
    UpdateStats _specificStats;

    // A user-initiated write is one which is not caused by oplog application and is not part of a
    // chunk migration
    bool _isUserInitiatedWrite;

    // These get reused for each update.
    mutablebson::Document& _doc;
    DamageVector _damages;

private:
    /**
     * Stores 'idToRetry' in '_idRetrying' so the update can be retried during the next call to
     * doWork(). Sets 'out' to WorkingSet::INVALID_ID.
     */
    void prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out);

    /**
     * Computes the result of applying mods to the document 'oldObj' at RecordId 'recordId' in
     * memory, then commits these changes to the database. Returns a possibly unowned copy
     * of the newly-updated version of the document.
     */
    BSONObj transformAndUpdate(const Snapshotted<BSONObj>& oldObj,
                               RecordId& recordId,
                               bool writeOnOrphan);

    // If not WorkingSet::INVALID_ID, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // If not WorkingSet::INVALID_ID, we return this member to our caller.
    WorkingSetID _idReturning;

    // Guard against the "Halloween Problem": If we're scanning an index {x:1} and performing
    // {$inc:{x:5}}, we'll keep moving the document forward and it will continue to reappear in our
    // index scan. Unless the index is multikey, the underlying query machinery won't de-dup so we
    // keep track of already updated docs in '_updatedRecordIds'.
    typedef stdx::unordered_set<RecordId, RecordId::Hasher> RecordIdSet;
    const std::unique_ptr<RecordIdSet> _updatedRecordIds;

    /**
     * This member is used to check whether the write should be performed, and if so, any other
     * behavior that should be done as part of the write (e.g. skipping it because it affects an
     * orphan document). A yield cannot happen between the check and the write, so the checks are
     * embedded in the stage.
     *
     * It's refreshed after yielding and reacquiring the locks.
     */
    write_stage_common::PreWriteFilter _preWriteFilter;
};

}  // namespace mongo
