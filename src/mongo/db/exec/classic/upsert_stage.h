// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/update_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Execution stage for update requests with {upsert:true}. This is a specialized UpdateStage which,
 * in the event that no documents match the update request's query, generates and inserts a new
 * document into the collection. All logic related to the insertion phase is implemented by this
 * class.
 *
 * If the prior or newly-updated version of the document was requested to be returned, then ADVANCED
 * is returned after updating or inserting a document. Otherwise, NEED_TIME is returned after
 * updating a document if further updates are pending, and IS_EOF is returned if all updates have
 * been performed or if a document has been inserted.
 *
 * Callers of doWork() must be holding a write lock.
 */
class UpsertStage final : public UpdateStage {
    UpsertStage(const UpsertStage&) = delete;
    UpsertStage& operator=(const UpsertStage&) = delete;

public:
    UpsertStage(ExpressionContext* expCtx,
                const UpdateStageParams& params,
                WorkingSet* ws,
                CollectionAcquisition collection,
                PlanStage* child);

    bool isEOF() const final;
    StageState doWork(WorkingSetID* out) final;

private:
    BSONObj _produceNewDocumentForInsert();

    // Performs the insert for the no-match-found branch of the upsert. Returns NEED_TIME once the
    // insert has committed, or NEED_YIELD if handlePlanStageYield caught a retryable storage
    // conflict (a WriteConflictException or similar), in which case the PlanExecutor will yield
    // (releasing the storage snapshot, locks, and ticket), back off, restore, and re-drive
    // doWork().
    PlanStage::StageState _performInsert(const BSONObj& newDocument, WorkingSetID* out);

    void _assertDocumentToBeInsertedIsValid(const mutablebson::Document& document,
                                            const FieldRefSet& shardKeyPaths);

    // The document to insert, produced once on the first insert attempt and reused across any
    // WriteConflictException retries so that the generated _id/OID remains stable. Its presence
    // also marks that doWork() is in the insert phase and should skip re-running
    // UpdateStage::doWork().
    boost::optional<BSONObj> _newDocumentToInsert;
};

}  // namespace mongo
