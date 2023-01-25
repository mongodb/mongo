/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/upsert_stage.h"

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/update/produce_document_for_upsert.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/s/would_change_owning_shard_exception.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeUpsertPerformsInsert);

namespace mb = mutablebson;

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

}  // namespace

UpsertStage::UpsertStage(ExpressionContext* expCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         const CollectionPtr& collection,
                         PlanStage* child)
    : UpdateStage(expCtx, params, ws, collection) {
    // We should never create this stage for a non-upsert request.
    invariant(_params.request->isUpsert());
    _children.emplace_back(child);
};

// We're done when updating is finished and we have either matched or inserted.
bool UpsertStage::isEOF() {
    return UpdateStage::isEOF() && (_specificStats.nMatched > 0 || _specificStats.nUpserted > 0);
}

PlanStage::StageState UpsertStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return StageState::IS_EOF;
    }

    boost::optional<repl::UnreplicatedWritesBlock> unReplBlock;
    if (collection()->ns().isImplicitlyReplicated()) {
        // Implictly replicated collections do not replicate updates.
        unReplBlock.emplace(opCtx());
    }

    // First, attempt to perform the update on a matching document.
    auto updateState = UpdateStage::doWork(out);

    //  If the update returned anything other than EOF, just forward it along. There's a chance we
    //  still may find a document to update and will not have to insert anything. If it did return
    //  EOF and we do not need to insert a new document, return EOF immediately here.
    if (updateState != PlanStage::IS_EOF || isEOF()) {
        return updateState;
    }

    // If the update resulted in EOF without matching anything, we must insert a new document.
    invariant(updateState == PlanStage::IS_EOF && !isEOF());

    // Since this is an insert, we will be logging it as such in the oplog. We don't need the
    // driver's help to build the oplog record. We also set the 'nUpserted' stats counter here.
    _params.driver->setLogOp(false);
    _specificStats.nUpserted = 1;

    // Generate the new document to be inserted.
    const auto& collDesc = _cachedShardingCollectionDescription.getCollectionDescription(opCtx());
    _specificStats.objInserted = update::produceDocumentForUpsert(opCtx(),
                                                                  _params.request,
                                                                  _params.driver,
                                                                  _params.canonicalQuery,
                                                                  _isUserInitiatedWrite,
                                                                  _doc,
                                                                  collDesc);

    // If this is an explain, skip performing the actual insert.
    if (!_params.request->explain()) {
        _performInsert(_specificStats.objInserted);
    }

    // We should always be EOF at this point.
    invariant(isEOF());

    // If we want to return the document we just inserted, create it as a WorkingSetMember.
    if (_params.request->shouldReturnNewDocs()) {
        BSONObj newObj = _specificStats.objInserted;
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->resetDocument(opCtx()->recoveryUnit()->getSnapshotId(), newObj.getOwned());
        member->transitionToOwnedObj();
        return PlanStage::ADVANCED;
    }

    // If we don't need to return the inserted document, we're done.
    return PlanStage::IS_EOF;
}

void UpsertStage::_performInsert(BSONObj newDocument) {
    // If this collection is sharded, check if the doc we plan to insert belongs to this shard. The
    // mongoS uses the query field to target a shard, and it is possible the shard key fields in the
    // 'q' field belong to this shard, but those in the 'u' field do not. In this case we need to
    // throw so that MongoS can target the insert to the correct shard.
    if (_isUserInitiatedWrite) {
        const auto& collDesc =
            _cachedShardingCollectionDescription.getCollectionDescription(opCtx());

        if (collDesc.isSharded()) {
            auto scopedCss = CollectionShardingState::assertCollectionLockedAndAcquire(
                opCtx(), collection()->ns());
            auto collFilter = scopedCss->getOwnershipFilter(
                opCtx(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);
            const ShardKeyPattern& shardKeyPattern = collFilter.getShardKeyPattern();
            auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newDocument);

            if (!collFilter.keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on another
                // shard must either be a retryable write or inside a transaction.
                // An upsert without a transaction number is legal if
                // gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because mongos
                // will be able to start an internal transaction to handle the
                // wouldChangeOwningShard error thrown below.
                if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
                        serverGlobalParams.featureCompatibility)) {
                    uassert(
                        ErrorCodes::IllegalOperation,
                        "The upsert document could not be inserted onto the shard targeted by the "
                        "query, since its shard key belongs on a different shard. Cross-shard "
                        "upserts are only allowed when running in a transaction or with "
                        "retryWrites: true.",
                        opCtx()->getTxnNumber());
                }
                uasserted(WouldChangeOwningShardInfo(_params.request->getQuery(),
                                                     newDocument,
                                                     true /* upsert */,
                                                     collection()->ns(),
                                                     collection()->uuid()),
                          "The document we are inserting belongs on a different shard");
            }
        }
    }

    if (MONGO_unlikely(hangBeforeUpsertPerformsInsert.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeUpsertPerformsInsert, opCtx(), "hangBeforeUpsertPerformsInsert");
    }

    writeConflictRetry(opCtx(), "upsert", collection()->ns().ns(), [&] {
        WriteUnitOfWork wunit(opCtx());
        InsertStatement insertStmt(_params.request->getStmtIds(), newDocument);

        auto replCoord = repl::ReplicationCoordinator::get(opCtx());
        if (collection()->isCapped() &&
            !replCoord->isOplogDisabledFor(opCtx(), collection()->ns())) {
            auto oplogInfo = LocalOplogInfo::get(opCtx());
            auto oplogSlots = oplogInfo->getNextOpTimes(opCtx(), /*batchSize=*/1);
            insertStmt.oplogSlot = oplogSlots.front();
        }

        uassertStatusOK(collection_internal::insertDocument(opCtx(),
                                                            collection(),
                                                            insertStmt,
                                                            _params.opDebug,
                                                            _params.request->source() ==
                                                                OperationSource::kFromMigrate));

        // Technically, we should save/restore state here, but since we are going to return
        // immediately after, it would just be wasted work.
        wunit.commit();
    });
}

}  // namespace mongo
