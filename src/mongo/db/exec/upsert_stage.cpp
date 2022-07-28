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

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
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
    _specificStats.objInserted = _produceNewDocumentForInsert();

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
        auto* const css = CollectionShardingState::get(opCtx(), collection()->ns());
        if (css->getCollectionDescription(opCtx()).isSharded()) {
            const auto collFilter = css->getOwnershipFilter(
                opCtx(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);
            const ShardKeyPattern& shardKeyPattern = collFilter.getShardKeyPattern();
            auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newDocument);

            if (!collFilter.keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on another
                // shard must either be a retryable write or inside a transaction.
                uassert(ErrorCodes::IllegalOperation,
                        "The upsert document could not be inserted onto the shard targeted by the "
                        "query, since its shard key belongs on a different shard. Cross-shard "
                        "upserts are only allowed when running in a transaction or with "
                        "retryWrites: true.",
                        opCtx()->getTxnNumber());
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

        uassertStatusOK(collection()->insertDocument(opCtx(),
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
    // Obtain the collection description. This will be needed to compute the shardKey paths.
    //
    // NOTE: The collection description must remain in scope since it owns the pointers used by
    // 'shardKeyPaths' and 'immutablePaths'.
    boost::optional<ScopedCollectionDescription> optCollDesc;
    FieldRefSet shardKeyPaths, immutablePaths;

    if (_isUserInitiatedWrite) {
        optCollDesc = CollectionShardingState::get(opCtx(), _params.request->getNamespaceString())
                          ->getCollectionDescription(opCtx());

        // If the collection is sharded, add all fields from the shard key to the 'shardKeyPaths'
        // set.
        if (optCollDesc->isSharded()) {
            shardKeyPaths.fillFrom(optCollDesc->getKeyPatternFields());
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

    // Reset the document into which we will be writing.
    _doc.reset();

    // First: populate the document's immutable paths with equality predicate values from the query,
    // if available. This generates the pre-image document that we will run the update against.
    if (auto* cq = _params.canonicalQuery) {
        uassertStatusOK(_params.driver->populateDocumentWithQueryFields(*cq, immutablePaths, _doc));
    } else {
        fassert(17354, CanonicalQuery::isSimpleIdQuery(_params.request->getQuery()));
        fassert(17352, _doc.root().appendElement(_params.request->getQuery()[idFieldName]));
    }

    // Second: run the appropriate document generation strategy over the document to generate the
    // post-image. If the update operation modifies any of the immutable paths, this will throw.
    if (_params.request->shouldUpsertSuppliedDocument()) {
        _generateNewDocumentFromSuppliedDoc(immutablePaths);
    } else {
        _generateNewDocumentFromUpdateOp(immutablePaths);
    }

    // Third: ensure _id is first if it exists, and generate a new OID otherwise.
    _ensureIdFieldIsFirst(&_doc, true);

    // Fourth: assert that the finished document has all required fields and is valid for storage.
    _assertDocumentToBeInsertedIsValid(_doc, shardKeyPaths);

    // Fifth: validate that the newly-produced document does not exceed the maximum BSON user size.
    auto newDocument = _doc.getObject();
    if (!DocumentValidationSettings::get(opCtx()).isInternalValidationDisabled()) {
        uassert(17420,
                str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
                newDocument.objsize() <= BSONObjMaxUserSize);
    }

    return newDocument;
}

void UpsertStage::_generateNewDocumentFromUpdateOp(const FieldRefSet& immutablePaths) {
    // Use the UpdateModification from the original request to generate a new document by running
    // the update over the empty (except for fields extracted from the query) document. We do not
    // validate for storage until later, but we do ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        _params.driver->update(opCtx(), {}, &_doc, validateForStorage, immutablePaths, isInsert));
};

void UpsertStage::_generateNewDocumentFromSuppliedDoc(const FieldRefSet& immutablePaths) {
    // We should never call this method unless the request has a set of update constants.
    invariant(_params.request->shouldUpsertSuppliedDocument());
    invariant(_params.request->getUpdateConstants());

    // Extract the supplied document from the constants and validate that it is an object.
    auto suppliedDocElt = _params.request->getUpdateConstants()->getField("new"_sd);
    invariant(suppliedDocElt.type() == BSONType::Object);
    auto suppliedDoc = suppliedDocElt.embeddedObject();

    // The supplied doc is functionally a replacement update. We need a new driver to apply it.
    UpdateDriver replacementDriver(nullptr);

    // Create a new replacement-style update from the supplied document.
    replacementDriver.parse(write_ops::UpdateModification::parseFromClassicUpdate(suppliedDoc), {});
    replacementDriver.setLogOp(false);

    // We do not validate for storage, as we will validate the full document before inserting.
    // However, we ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        replacementDriver.update(opCtx(), {}, &_doc, validateForStorage, immutablePaths, isInsert));
}

void UpsertStage::_assertDocumentToBeInsertedIsValid(const mb::Document& document,
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
        _assertPathsNotArray(document, {{&idFieldRef}});
        _assertPathsNotArray(document, shardKeyPaths);
    }
}

}  // namespace mongo
