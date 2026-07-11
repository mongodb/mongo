// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

class OpDebug;
struct PlanSummaryStats;

struct [[MONGO_MOD_PUBLIC]] UpdateStageParams {
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
    static constexpr const char* kStageType = "UPDATE";

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
    const std::unique_ptr<update::RecordIdSet> _updatedRecordIds;

    // Check memory usage of the stage.
    SimpleMemoryUsageTracker _memoryTracker;

    // Tracks memory usage of the record ID deduplicator and reports metrics to serverStatus.
    DeduplicatorReporter _dedupReporter;

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
