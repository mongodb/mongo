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

#include "mongo/db/exec/classic/upsert_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/local_oplog_info.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/db/update/update_util.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/str.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeUpsertPerformsInsert);

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

}  // namespace

UpsertStage::UpsertStage(ExpressionContext* expCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         CollectionAcquisition collection,
                         PlanStage* child)
    : UpdateStage(expCtx, params, ws, collection) {
    // We should never create this stage for a non-upsert request.
    invariant(_params.request->isUpsert());
    _children.emplace_back(child);
};

// We're done when updating is finished and we have either matched or inserted.
bool UpsertStage::isEOF() const {
    return UpdateStage::isEOF() && (_specificStats.nMatched > 0 || _specificStats.nUpserted > 0);
}

PlanStage::StageState UpsertStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return StageState::IS_EOF;
    }

    boost::optional<repl::UnreplicatedWritesBlock> unReplBlock;
    if (collectionPtr()->ns().isImplicitlyReplicated()) {
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
    // driver's help to build the oplog record.
    _params.driver->setLogOp(false);

    // Generate the new document to be inserted.
    auto newObj = _produceNewDocumentForInsert();

    // If this is an explain, skip performing the actual insert.
    if (!_params.request->explain()) {
        _performInsert(newObj);
    }

    _specificStats.objInserted = newObj;
    _specificStats.nUpserted = 1;

    // We should always be EOF at this point.
    invariant(isEOF());

    // If we want to return the document we just inserted, create it as a WorkingSetMember.
    if (_params.request->shouldReturnNewDocs()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->resetDocument(shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId(),
                              newObj.getOwned());
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
        const auto& collDesc = collectionAcquisition().getShardingDescription();

        if (collDesc.isSharded()) {
            const auto& collFilter = collectionAcquisition().getShardingFilter();
            invariant(collFilter);
            auto newShardKey = collDesc.getShardKeyPattern().extractShardKeyFromDoc(newDocument);

            if (!collFilter->keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on another
                // shard must either be a retryable write or inside a transaction.
                // An upsert without a transaction number is legal if
                // gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because mongos
                // will be able to start an internal transaction to handle the
                // wouldChangeOwningShard error thrown below.
                if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
                        VersionContext::getDecoration(opCtx()),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
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
                                                     collectionPtr()->ns(),
                                                     collectionPtr()->uuid()),
                          "The document we are inserting belongs on a different shard");
            }
        }
    }

    if (MONGO_unlikely(hangBeforeUpsertPerformsInsert.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeUpsertPerformsInsert, opCtx(), "hangBeforeUpsertPerformsInsert");
    }

    writeConflictRetry(opCtx(), "upsert", collectionPtr()->ns(), [&] {
        WriteUnitOfWork wunit(opCtx());
        InsertStatement insertStmt(_params.request->getStmtIds(), newDocument);

        auto replCoord = repl::ReplicationCoordinator::get(opCtx());
        if (collectionPtr()->isCapped() &&
            !replCoord->isOplogDisabledFor(opCtx(), collectionPtr()->ns())) {
            if (collectionPtr()->needsCappedLock()) {
                Lock::ResourceLock heldUntilEndOfWUOW{
                    opCtx(), ResourceId(RESOURCE_METADATA, collectionPtr()->ns()), MODE_X};
            }
            auto oplogInfo = LocalOplogInfo::get(opCtx());
            auto oplogSlots = oplogInfo->getNextOpTimes(opCtx(), /*batchSize=*/1);
            insertStmt.oplogSlot = oplogSlots.front();
        }

        uassertStatusOK(collection_internal::insertDocument(opCtx(),
                                                            collectionPtr(),
                                                            insertStmt,
                                                            _params.opDebug,
                                                            _params.request->source() ==
                                                                OperationSource::kFromMigrate));

        // Technically, we should save/restore state here, but since we are going to return
        // immediately after, it would just be wasted work.
        wunit.commit();
    });
}

BSONObj UpsertStage::_produceNewDocumentForInsert() {
    FieldRefSet shardKeyPaths, immutablePaths;
    if (_isUserInitiatedWrite) {
        // Obtain the collection description. This will be needed to compute the shardKey paths.
        const auto& collDesc = collectionAcquisition().getShardingDescription();

        // If the collection is sharded, add all fields from the shard key to the 'shardKeyPaths'
        // set.
        if (collDesc.isSharded()) {
            shardKeyPaths.fillFrom(collDesc.getKeyPatternFields());
        }

        // An unversioned request cannot update the shard key, so all shardKey paths are immutable.
        if (!OperationShardingState::isComingFromRouter(opCtx())) {
            for (auto&& shardKeyPath : shardKeyPaths) {
                immutablePaths.insert(shardKeyPath);
            }
        }

        // The _id field is always immutable to user requests, even if the shard key is mutable.
        immutablePaths.keepShortest(&idFieldRef);
    }

    // Generate the new document to be inserted.
    update::produceDocumentForUpsert(
        opCtx(), _params.request, _params.driver, _params.canonicalQuery, immutablePaths, _doc);

    // Assert that the finished document has all required fields and is valid for storage.
    _assertDocumentToBeInsertedIsValid(_doc, shardKeyPaths);

    auto newDocument = _doc.getObject();
    if (!DocumentValidationSettings::get(opCtx()).isInternalValidationDisabled()) {
        uassert(17420,
                str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
                newDocument.objsize() <= BSONObjMaxUserSize);
    }

    return newDocument;
}

void UpsertStage::_assertDocumentToBeInsertedIsValid(const mutablebson::Document& document,
                                                     const FieldRefSet& shardKeyPaths) {
    // For a non-internal operation, we assert that the document contains all required paths, that
    // no shard key fields have arrays at any point along their paths, and that the document is
    // valid for storage. Skip all such checks for an internal operation.
    if (_isUserInitiatedWrite) {
        // Shard key values are permitted to be missing, and so the only required field is _id. We
        // should always have an _id here, since we generated one earlier if not already present.
        invariant(document.root().ok() && document.root()[idFieldName].ok());
        bool containsDotsAndDollarsField = false;

        storage_validation::scanDocument(document,
                                         true, /* allowTopLevelDollarPrefixes */
                                         true, /* Should validate for storage */
                                         &containsDotsAndDollarsField);
        if (containsDotsAndDollarsField)
            _params.driver->setContainsDotsAndDollarsField(true);

        //  Neither _id nor the shard key fields may have arrays at any point along their paths.
        update::assertPathsNotArray(document, {{&idFieldRef}});
        update::assertPathsNotArray(document, shardKeyPaths);
    }
}

}  // namespace mongo
