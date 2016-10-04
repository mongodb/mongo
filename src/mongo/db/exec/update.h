/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"

namespace mongo {

class OperationContext;
class OpDebug;
struct PlanSummaryStats;

struct UpdateStageParams {
    UpdateStageParams(const UpdateRequest* r, UpdateDriver* d, OpDebug* o)
        : request(r), driver(d), opDebug(o), canonicalQuery(NULL) {}

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

private:
    // Default constructor not allowed.
    UpdateStageParams();
};

/**
 * Execution stage responsible for updates to documents and upserts. If the prior or
 * newly-updated version of the document was requested to be returned, then ADVANCED is
 * returned after updating or inserting a document. Otherwise, NEED_TIME is returned after
 * updating or inserting a document.
 *
 * Callers of work() must be holding a write lock.
 */
class UpdateStage final : public PlanStage {
    MONGO_DISALLOW_COPYING(UpdateStage);

public:
    UpdateStage(OperationContext* txn,
                const UpdateStageParams& params,
                WorkingSet* ws,
                Collection* collection,
                PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    void doRestoreState() final;

    StageType stageType() const final {
        return STAGE_UPDATE;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

    /**
     * Gets a pointer to the UpdateStats inside 'exec'.
     *
     * The 'exec' must have an UPDATE stage as its root stage, and the plan must be EOF before
     * calling this method.
     */
    static const UpdateStats* getUpdateStats(const PlanExecutor* exec);

    /**
     * Populate 'opDebug' with stats from 'updateStats' describing the execution of this update.
     */
    static void recordUpdateStatsInOpDebug(const UpdateStats* updateStats, OpDebug* opDebug);

    /**
     * Converts 'updateStats' into an UpdateResult.
     */
    static UpdateResult makeUpdateResult(const UpdateStats* updateStats);

    /**
     * Computes the document to insert if the upsert flag is set to true and no matching
     * documents are found in the database. The document to upsert is computing using the
     * query 'cq' and the update mods contained in 'driver'.
     *
     * If 'cq' is NULL, which can happen for the idhack update fast path, then 'query' is
     * used to compute the doc to insert instead of 'cq'.
     *
     * 'doc' is the mutable BSON document which you would like the update driver to use
     * when computing the document to insert.
     *
     * Set 'isInternalRequest' to true if the upsert was issued by the replication or
     * sharding systems.
     *
     * Fills out whether or not this is a fastmodinsert in 'stats'.
     *
     * Returns the document to insert in *out.
     */
    static Status applyUpdateOpsForInsert(OperationContext* txn,
                                          const CanonicalQuery* cq,
                                          const BSONObj& query,
                                          UpdateDriver* driver,
                                          mutablebson::Document* doc,
                                          bool isInternalRequest,
                                          const NamespaceString& ns,
                                          UpdateStats* stats,
                                          BSONObj* out);

private:
    /**
     * Computes the result of applying mods to the document 'oldObj' at RecordId 'recordId' in
     * memory, then commits these changes to the database. Returns a possibly unowned copy
     * of the newly-updated version of the document.
     */
    BSONObj transformAndUpdate(const Snapshotted<BSONObj>& oldObj, RecordId& recordId);

    /**
     * Computes the document to insert and inserts it into the collection. Used if the
     * user requested an upsert and no matching documents were found.
     */
    void doInsert();

    /**
     * Have we performed all necessary updates? Even if this is true, we might not be EOF,
     * as we might still have to do an insert.
     */
    bool doneUpdating();

    /**
     * Examines the stats / update request and returns whether there is still an insert left
     * to do. If so then this stage is not EOF yet.
     */
    bool needInsert();

    /**
     * Helper for restoring the state of this update.
     */
    Status restoreUpdateState();

    /**
     * Stores 'idToRetry' in '_idRetrying' so the update can be retried during the next call to
     * work(). Always returns NEED_YIELD and sets 'out' to WorkingSet::INVALID_ID.
     */
    StageState prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out);

    UpdateStageParams _params;

    // Not owned by us.
    WorkingSet* _ws;

    // Not owned by us. May be NULL.
    Collection* _collection;

    // If not WorkingSet::INVALID_ID, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // If not WorkingSet::INVALID_ID, we return this member to our caller.
    WorkingSetID _idReturning;

    // Stats
    UpdateStats _specificStats;

    // If the update was in-place, we may see it again.  This only matters if we're doing
    // a multi-update; if we're not doing a multi-update we stop after one update and we
    // won't see any more docs.
    //
    // For example: If we're scanning an index {x:1} and performing {$inc:{x:5}}, we'll keep
    // moving the document forward and it will continue to reappear in our index scan.
    // Unless the index is multikey, the underlying query machinery won't de-dup.
    //
    // If the update wasn't in-place we may see it again.  Our query may return the new
    // document and we wouldn't want to update that.
    //
    // So, no matter what, we keep track of where the doc wound up.
    typedef unordered_set<RecordId, RecordId::Hasher> RecordIdSet;
    const std::unique_ptr<RecordIdSet> _updatedRecordIds;

    // These get reused for each update.
    mutablebson::Document& _doc;
    mutablebson::DamageVector _damages;
};

}  // namespace mongo
