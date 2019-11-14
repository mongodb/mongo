/**
 *    Copyright (C) 2019 MongoDB, Inc.
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

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/s/would_change_owning_shard_exception.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeUpsertPerformsInsert);

namespace mb = mutablebson;

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

/**
 * Populates the given FieldRefSets with the shard key paths (if applicable) and all paths which
 * are not modifiable, respectively. The contents of these two sets may or may not be identical.
 */
void getShardKeyAndImmutablePaths(OperationContext* opCtx,
                                  const ScopedCollectionMetadata& metadata,
                                  bool isInternalRequest,
                                  FieldRefSet* shardKeyPaths,
                                  FieldRefSet* immutablePaths) {
    // If the collection is sharded, add all fields from the shard key to the 'shardKeyPaths' set.
    if (metadata->isSharded()) {
        shardKeyPaths->fillFrom(metadata->getKeyPatternFields());
    }
    // If this is an internal request, no fields are immutable and we leave 'immutablePaths' empty.
    if (!isInternalRequest) {
        // An unversioned request cannot update the shard key, so all shardKey paths are immutable.
        if (!OperationShardingState::isOperationVersioned(opCtx)) {
            for (auto&& shardKeyPath : *shardKeyPaths) {
                immutablePaths->insert(shardKeyPath);
            }
        }
        // The _id field is always immutable to user requests, even if the shard key is mutable.
        immutablePaths->keepShortest(&idFieldRef);
    }
}
}  // namespace

UpsertStage::UpsertStage(OperationContext* opCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : UpdateStage(opCtx, params, ws, collection) {
    // We should never create this stage for a non-upsert request.
    invariant(_params.request->isUpsert());
    _children.emplace_back(child);
};

// We're done when updating is finished and we have either matched or inserted.
bool UpsertStage::isEOF() {
    return UpdateStage::isEOF() && (_specificStats.nMatched > 0 || _specificStats.inserted);
}

PlanStage::StageState UpsertStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return StageState::IS_EOF;
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
    // driver's help to build the oplog record. We also set the 'inserted' stats flag here.
    _params.driver->setLogOp(false);
    _specificStats.inserted = true;

    // Determine whether this is a user-initiated or internal request.
    const bool isInternalRequest =
        !getOpCtx()->writesAreReplicated() || _params.request->isFromMigration();

    // Generate the new document to be inserted.
    _specificStats.objInserted = _produceNewDocumentForInsert(isInternalRequest);

    // If this is an explain, skip performing the actual insert.
    if (!_params.request->isExplain()) {
        _performInsert(_specificStats.objInserted);
    }

    // We should always be EOF at this point.
    invariant(isEOF());

    // If we want to return the document we just inserted, create it as a WorkingSetMember.
    if (_params.request->shouldReturnNewDocs()) {
        BSONObj newObj = _specificStats.objInserted;
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->resetDocument(getOpCtx()->recoveryUnit()->getSnapshotId(), newObj.getOwned());
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
    if (_shouldCheckForShardKeyUpdate) {
        auto* const css = CollectionShardingState::get(getOpCtx(), collection()->ns());
        const auto& metadata = css->getCurrentMetadata();

        if (metadata->isSharded()) {
            const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
            auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newDocument);

            if (!metadata->keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on another
                // shard must either be a retryable write or inside a transaction.
                uassert(ErrorCodes::IllegalOperation,
                        "The upsert document could not be inserted onto the shard targeted by the "
                        "query, since its shard key belongs on a different shard. Cross-shard "
                        "upserts are only allowed when running in a transaction or with "
                        "retryWrites: true.",
                        getOpCtx()->getTxnNumber());
                uasserted(WouldChangeOwningShardInfo(
                              _params.request->getQuery(), newDocument, true /* upsert */),
                          "The document we are inserting belongs on a different shard");
            }
        }
    }

    if (MONGO_unlikely(hangBeforeUpsertPerformsInsert.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeUpsertPerformsInsert, getOpCtx(), "hangBeforeUpsertPerformsInsert");
    }

    writeConflictRetry(getOpCtx(), "upsert", collection()->ns().ns(), [&] {
        WriteUnitOfWork wunit(getOpCtx());
        uassertStatusOK(
            collection()->insertDocument(getOpCtx(),
                                         InsertStatement(_params.request->getStmtId(), newDocument),
                                         _params.opDebug,
                                         _params.request->isFromMigration()));

        // Technically, we should save/restore state here, but since we are going to return
        // immediately after, it would just be wasted work.
        wunit.commit();
    });
}

BSONObj UpsertStage::_produceNewDocumentForInsert(bool isInternalRequest) {
    // Obtain the sharding metadata. This will be needed to compute the shardKey paths. The metadata
    // must remain in scope since it owns the pointers used by 'shardKeyPaths' and 'immutablePaths'.
    auto* css = CollectionShardingState::get(getOpCtx(), _params.request->getNamespaceString());
    auto metadata = css->getCurrentMetadata();

    // Compute the set of shard key paths and the set of immutable paths. Either may be empty.
    FieldRefSet shardKeyPaths, immutablePaths;
    getShardKeyAndImmutablePaths(
        getOpCtx(), metadata, isInternalRequest, &shardKeyPaths, &immutablePaths);

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
    _assertDocumentToBeInsertedIsValid(
        _doc, shardKeyPaths, isInternalRequest, _enforceOkForStorage);

    // Fifth: validate that the newly-produced document does not exceed the maximum BSON user size.
    auto newDocument = _doc.getObject();
    uassert(17420,
            str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
            newDocument.objsize() <= BSONObjMaxUserSize);

    return newDocument;
}

void UpsertStage::_generateNewDocumentFromUpdateOp(const FieldRefSet& immutablePaths) {
    // Use the UpdateModification from the original request to generate a new document by running
    // the update over the empty (except for fields extracted from the query) document. We do not
    // validate for storage until later, but we do ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        _params.driver->update({}, &_doc, validateForStorage, immutablePaths, isInsert));
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
    replacementDriver.parse({suppliedDoc}, {});
    replacementDriver.setLogOp(false);

    // We do not validate for storage, as we will validate the full document before inserting.
    // However, we ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        replacementDriver.update({}, &_doc, validateForStorage, immutablePaths, isInsert));
}

void UpsertStage::_assertDocumentToBeInsertedIsValid(const mb::Document& document,
                                                     const FieldRefSet& shardKeyPaths,
                                                     bool isInternalRequest,
                                                     bool enforceOkForStorage) {
    // For a non-internal operation, we assert that the document contains all required paths, that
    // no shard key fields have arrays at any point along their paths, and that the document is
    // valid for storage. Skip all such checks for an internal operation.
    if (!isInternalRequest) {
        if (enforceOkForStorage) {
            storage_validation::storageValid(document);
        }
        // Shard key values are permitted to be missing, and so the only required field is _id. We
        // should always have an _id here, since we generated one earlier if not already present.
        invariant(document.root().ok() && document.root()[idFieldName].ok());

        //  Neither _id nor the shard key fields may have arrays at any point along their paths.
        _assertPathsNotArray(document, {{&idFieldRef}});
        _assertPathsNotArray(document, shardKeyPaths);
    }
}
}  // namespace mongo
