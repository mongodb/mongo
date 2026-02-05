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

#include "mongo/db/update/update_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/matcher/match_details.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <map>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {
namespace update {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeThrowWouldChangeOwningShard);

constexpr StringData idFieldName = "_id"_sd;
const FieldRef idFieldRef(idFieldName);

}  // namespace

void addObjectIDIdField(mutablebson::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    uassert(17268, "Could not create new ObjectId '_id' field.", idElem.ok());
    uassertStatusOK(doc->root().pushFront(idElem));
}

void generateNewDocumentFromUpdateOp(OperationContext* opCtx,
                                     const FieldRefSet& immutablePaths,
                                     UpdateDriver* driver,
                                     mutablebson::Document& document) {
    // Use the UpdateModification from the original request to generate a new document by running
    // the update over the empty (except for fields extracted from the query) document. We do not
    // validate for storage until later, but we do ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        driver->update(opCtx, {}, &document, validateForStorage, immutablePaths, isInsert));
};

void generateNewDocumentFromSuppliedDoc(OperationContext* opCtx,
                                        const FieldRefSet& immutablePaths,
                                        const UpdateRequest* request,
                                        mutablebson::Document& document) {
    // We should never call this method unless the request has a set of update constants.
    invariant(request->shouldUpsertSuppliedDocument());
    invariant(request->getUpdateConstants());

    // Extract the supplied document from the constants and validate that it is an object.
    auto suppliedDocElt = request->getUpdateConstants()->getField("new"_sd);
    invariant(suppliedDocElt.type() == BSONType::object);
    auto suppliedDoc = suppliedDocElt.embeddedObject();

    // The supplied doc is functionally a replacement update. We need a new driver to apply it.
    UpdateDriver replacementDriver(nullptr);

    // Create a new replacement-style update from the supplied document.
    replacementDriver.parse(
        write_ops::UpdateModification(suppliedDoc, write_ops::UpdateModification::ReplacementTag{}),
        {});
    replacementDriver.setLogOp(false);
    replacementDriver.setBypassEmptyTsReplacement(
        static_cast<bool>(request->getBypassEmptyTsReplacement()));

    // We do not validate for storage, as we will validate the full document before inserting.
    // However, we ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(replacementDriver.update(
        opCtx, {}, &document, validateForStorage, immutablePaths, isInsert));
}

void produceDocumentForUpsert(OperationContext* opCtx,
                              const UpdateRequest* request,
                              UpdateDriver* driver,
                              const CanonicalQuery* canonicalQuery,
                              const FieldRefSet& immutablePaths,
                              mutablebson::Document& doc) {
    // Reset the document into which we will be writing.
    doc.reset();

    // First: populate the document's immutable paths with equality predicate values from the query,
    // if available. This generates the pre-image document that we will run the update against.
    if (auto* cq = canonicalQuery) {
        uassertStatusOK(driver->populateDocumentWithQueryFields(
            *cq->getPrimaryMatchExpression(), immutablePaths, doc));
    } else {
        fassert(17354, isSimpleIdQuery(request->getQuery()));
        // IDHACK path allows for queries of the shape {_id: 123} and {_id: {$eq: 123}}. Neither
        // case will have generated a CanonicalQuery earlier, so we have to figure out which value
        // should be in the created document here, since we cannot insert a document that looks like
        // {_id: {$eq: 123}}.
        const BSONElement& idVal = request->getQuery()[idFieldName];
        if (idVal.isABSONObj() && idVal.Obj().hasField("$eq")) {
            // We append an element of the shape {_id: 123}.
            mutablebson::Element newElement =
                doc.makeElementWithNewFieldName(idFieldName, idVal["$eq"]);
            fassert(9248800, newElement.ok());
            fassert(9248803, doc.root().pushBack(newElement));
        } else {
            fassert(17352, doc.root().appendElement(idVal));
        }
    }

    // Second: run the appropriate document generation strategy over the document to generate the
    // post-image. If the update operation modifies any of the immutable paths, this will throw.
    if (request->shouldUpsertSuppliedDocument()) {
        generateNewDocumentFromSuppliedDoc(opCtx, immutablePaths, request, doc);
    } else {
        generateNewDocumentFromUpdateOp(opCtx, immutablePaths, driver, doc);
    }

    // Third: ensure _id is first if it exists, and generate a new OID otherwise.
    ensureIdFieldIsFirst(&doc, true);
}

void assertPathsNotArray(const mutablebson::Document& document, const FieldRefSet& paths) {
    for (const auto& path : paths) {
        auto elem = document.root();
        // If any path component does not exist, we stop checking for arrays along the path.
        for (size_t i = 0; elem.ok() && i < (*path).numParts(); ++i) {
            elem = elem[(*path).getPart(i)];
            uassert(ErrorCodes::NotSingleValueField,
                    str::stream() << "After applying the update to the document, the field '"
                                  << (*path).dottedField()
                                  << "' was found to be an array or array descendant.",
                    !elem.ok() || elem.getType() != BSONType::array);
        }
    }
}

void ensureIdFieldIsFirst(mutablebson::Document* doc, bool generateOIDIfMissing) {
    mutablebson::Element idElem = mutablebson::findFirstChildNamed(doc->root(), idFieldName);

    // If the document has no _id and the caller has requested that we generate one, do so.
    if (!idElem.ok() && generateOIDIfMissing) {
        addObjectIDIdField(doc);
    } else if (idElem.ok() && idElem.leftSibling().ok()) {
        // If the document does have an _id but it is not the first element, move it to the front.
        uassertStatusOK(idElem.remove());
        uassertStatusOK(doc->root().pushFront(idElem));
    }
}

void makeUpdateRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       boost::optional<ExplainOptions::Verbosity> explain,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    invariant(request.getUpdate());
    requestOut->setUpdateModification(*request.getUpdate());
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLetParameters(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setArrayFilters(request.getArrayFilters().value_or(std::vector<BSONObj>()));
    requestOut->setUpsert(request.getUpsert().value_or(false));
    requestOut->setReturnDocs((request.getNew().value_or(false)) ? UpdateRequest::RETURN_NEW
                                                                 : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setExplain(explain);

    requestOut->setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    requestOut->setIsTimeseriesNamespace(request.getIsTimeseriesNamespace());
    requestOut->setBypassEmptyTsReplacement(request.getBypassEmptyTsReplacement());
}

void ShardingChecksForUpdate::_checkRestrictionsOnUpdatingShardKeyAreNotViolated(
    OperationContext* opCtx,
    const ScopedCollectionDescription& collDesc,
    const FieldRefSet& shardKeyPaths) {
    // We do not allow updates to the shard key when 'multi' is true.
    uassert(ErrorCodes::InvalidOptions,
            "Multi-update operations are not allowed when updating the shard key field.",
            !_isMulti);

    // $_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter, used exclusively
    // by the two-phase write protocol for updateOne without shard key.
    if (_allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
        bool isInternalThreadOrClient =
            !Client::getCurrent()->session() || Client::getCurrent()->isInternalClient();
        uassert(ErrorCodes::InvalidOptions,
                "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter",
                isInternalThreadOrClient);
    }

    // If this node is a replica set primary node, an attempted update to the shard key value must
    // either be a retryable write or inside a transaction. An update without a transaction number
    // is legal if gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because mongos
    // will be able to start an internal transaction to handle the wouldChangeOwningShard error
    // thrown below. If this node is a replica set secondary node, we can skip validation.
    if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // As of v7.1, MongoDB supports WCOS updates without having a full shard key in the filter,
        // provided the update is retryable or in transaction. Normally the shards can enforce this
        // rule by simply checking 'opCtx'.  However, for some write types, the router creates an
        // internal transaction and runs commands on the shards inside this transaction. To ensure
        // WCOS updates are only allowed if the original command was retryable or transaction, the
        // router will explicitly set $_allowShardKeyUpdatesWithoutFullShardKeyInQuery to true or
        // false to instruct whether WCOS updates should be allowed.
        uassert(ErrorCodes::IllegalOperation,
                "Must run update to shard key field in a multi-statement transaction or with "
                "retryWrites: true.",
                _allowShardKeyUpdatesWithoutFullShardKeyInQuery.value_or(
                    static_cast<bool>(opCtx->getTxnNumber())));
    }
}

void ShardingChecksForUpdate::checkUpdateChangesReshardingKey(OperationContext* opCtx,
                                                              const BSONObj& newObj,
                                                              const Snapshotted<BSONObj>& oldObj) {

    auto& reshardingPlacement = _collAcq.getPostReshardingPlacement();
    if (!reshardingPlacement)
        return;
    auto oldShardKey = reshardingPlacement->extractReshardingKeyFromDocument(oldObj.value());
    auto newShardKey = reshardingPlacement->extractReshardingKeyFromDocument(newObj);

    if (newShardKey.binaryEqual(oldShardKey))
        return;

    const auto& collDesc = _collAcq.getShardingDescription();
    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());
    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(opCtx, collDesc, shardKeyPaths);

    auto oldRecipShard =
        reshardingPlacement->getReshardingDestinedRecipientFromShardKey(oldShardKey);
    auto newRecipShard =
        reshardingPlacement->getReshardingDestinedRecipientFromShardKey(newShardKey);

    uassert(WouldChangeOwningShardInfo(oldObj.value(),
                                       newObj,
                                       false /* upsert */,
                                       _collAcq.nss(),
                                       _collAcq.uuid(),
                                       boost::none,
                                       oldRecipShard),
            "This update would cause the doc to change owning shards under the new shard key",
            oldRecipShard == newRecipShard);
}

void ShardingChecksForUpdate::checkUpdateChangesShardKeyFields(
    OperationContext* opCtx,
    const mutablebson::Document& newDoc,
    const boost::optional<BSONObj>& newObjCopy,
    const Snapshotted<BSONObj>& oldObj) {
    // Calling mutablebson::Document::getObject() renders a full copy of the updated document. This
    // can be expensive for larger documents, so we skip calling it when the collection isn't even
    // sharded.
    const auto isSharded = _collAcq.getShardingDescription().isSharded();
    if (!isSharded) {
        return;
    }

    const auto& newObj = newObjCopy ? *newObjCopy : newDoc.getObject();
    // It is possible that both the existing and new shard keys are being updated, so we do not want
    // to short-circuit checking whether either is being modified.
    checkUpdateChangesExistingShardKey(opCtx, newDoc, newObj, oldObj);
    checkUpdateChangesReshardingKey(opCtx, newObj, oldObj);
}

void ShardingChecksForUpdate::checkUpdateChangesExistingShardKey(
    OperationContext* opCtx,
    const mutablebson::Document& newDoc,
    const BSONObj& newObj,
    const Snapshotted<BSONObj>& oldObj) {

    const auto& collDesc = _collAcq.getShardingDescription();
    const auto& shardKeyPattern = collDesc.getShardKeyPattern();

    auto oldShardKey = shardKeyPattern.extractShardKeyFromDoc(oldObj.value());
    auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newObj);

    // If the shard key fields remain unchanged by this update we can skip the rest of the checks.
    // Using BSONObj::binaryEqual() still allows a missing shard key field to be filled in with an
    // explicit null value.
    if (newShardKey.binaryEqual(oldShardKey)) {
        return;
    }

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());

    // Assert that the updated doc has no arrays or array descendants for the shard key fields.
    update::assertPathsNotArray(newDoc, shardKeyPaths);

    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(opCtx, collDesc, shardKeyPaths);

    // At this point we already asserted that the complete shardKey have been specified in the
    // query, this implies that mongos is not doing a broadcast update and that it attached a
    // shardVersion to the command. Thus it is safe to call getOwnershipFilter
    const auto& collFilter = _collAcq.getShardingFilter();
    invariant(collFilter);

    // If the shard key of an orphan document is allowed to change, and the document is allowed to
    // become owned by the shard, the global uniqueness assumption for _id values would be violated.
    invariant(collFilter->keyBelongsToMe(oldShardKey));

    if (!collFilter->keyBelongsToMe(newShardKey)) {
        if (MONGO_unlikely(hangBeforeThrowWouldChangeOwningShard.shouldFail())) {
            LOGV2(20605, "Hit hangBeforeThrowWouldChangeOwningShard failpoint");
            hangBeforeThrowWouldChangeOwningShard.pauseWhileSet(opCtx);
        }

        auto& collectionPtr = _collAcq.getCollectionPtr();
        uasserted(WouldChangeOwningShardInfo(oldObj.value(),
                                             newObj,
                                             false /* upsert */,
                                             collectionPtr->ns(),
                                             collectionPtr->uuid()),
                  "This update would cause the doc to change owning shards");
    }
}

std::pair<BSONObj, bool> transformDocument(OperationContext* opCtx,
                                           const CollectionAcquisition& collection,
                                           const Snapshotted<BSONObj>& oldObj,
                                           mutablebson::Document& doc,
                                           bool isUserInitiatedWrite,
                                           CanonicalQuery* cq,
                                           const RecordId& rid,
                                           UpdateDriver* driver,
                                           const UpdateRequest* request,
                                           bool shouldWriteToOrphan,
                                           RecordIdSet* updatedRecordIds,
                                           const SeekableRecordCursor* cursor) {
    Status status = Status::OK();

    const BSONObj& oldObjValue = oldObj.value();

    // Ask the driver to apply the mods. It may be that the driver can apply those "in
    // place", that is, some values of the old document just get adjusted without any
    // change to the binary layout on the bson layer. It may be that a whole new document
    // is needed to accommodate the new bson layout of the resulting document. In any event,
    // only enable in-place mutations if the underlying storage engine offers support for
    // writing damage events.
    doc.reset(oldObjValue,
              (collection.getCollectionPtr()->updateWithDamagesSupported()
                   ? mutablebson::Document::kInPlaceEnabled
                   : mutablebson::Document::kInPlaceDisabled));

    FieldRefSet immutablePaths;
    if (isUserInitiatedWrite) {
        const auto& collDesc = collection.getShardingDescription();
        if (collDesc.isSharded() && !OperationShardingState::isVersioned(opCtx, collection.nss())) {
            immutablePaths.fillFrom(collDesc.getKeyPatternFields());
        }
        immutablePaths.keepShortest(&idFieldRef);
    }

    std::string matchedField = "";
    if (driver->needMatchDetails()) {
        tassert(11534100, "Positional updates require a canonical query", cq);
        // If there was a matched field, obtain it.
        MatchDetails matchDetails;
        matchDetails.requestElemMatchKey();

        MONGO_verify(exec::matcher::matchesBSON(
            cq->getPrimaryMatchExpression(), oldObjValue, &matchDetails));

        if (matchDetails.hasElemMatchKey())
            matchedField = matchDetails.elemMatchKey();
    }

    BSONObj logObj;
    bool docWasModified = false;
    status = driver->update(opCtx,
                            matchedField,
                            &doc,
                            isUserInitiatedWrite,
                            immutablePaths,
                            false, /* isInsert */
                            &logObj,
                            &docWasModified);
    uassertStatusOK(status);

    // Skip adding _id field if the collection is capped (since capped collection documents can
    // neither grow nor shrink).
    const auto createIdField = !collection.getCollectionPtr()->isCapped();
    // Ensure _id is first if it exists, and generate a new OID if appropriate.
    update::ensureIdFieldIsFirst(&doc, createIdField);

    DamageVector damages;
    const char* source = nullptr;
    const bool inPlace = doc.getInPlaceUpdates(&damages, &source);

    if (inPlace && damages.empty()) {
        // A modifier didn't notice that it was really a no-op during its 'prepare' phase. That
        // represents a missed optimization, but we still shouldn't do any real work. Toggle
        // 'docWasModified' to 'false'.
        //
        // Currently, an example of this is '{ $push : { x : {$each: [], $sort: 1} } }' when the
        // 'x' array exists and is already sorted.
        docWasModified = false;
    }

    if (!docWasModified) {
        // Explains should always be treated as if they wrote.
        return {oldObjValue, request->explain().has_value()};
    }

    // Prepare to modify the document
    CollectionUpdateArgs args{oldObjValue};
    args.update = logObj;
    if (isUserInitiatedWrite) {
        const auto& collDesc = collection.getShardingDescription();
        args.criteria = collDesc.extractDocumentKey(oldObj.value());
    } else {
        const auto docId = oldObj.value()[idFieldName];
        args.criteria = docId ? docId.wrap() : oldObj.value();
    }
    uassert(11533703,
            "Multi-update operations require all documents to have an '_id' field",
            !request->isMulti() || args.criteria.hasField(idFieldName));

    args.source = shouldWriteToOrphan ? OperationSource::kFromMigrate : request->source();
    args.stmtIds = request->getStmtIds();
    args.sampleId = request->getSampleId();

    if (request->getReturnDocs() == UpdateRequest::ReturnDocOption::RETURN_NEW) {
        args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PostImage;
    } else if (request->getReturnDocs() == UpdateRequest::ReturnDocOption::RETURN_OLD) {
        args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;
    } else {
        args.storeDocOption = CollectionUpdateArgs::StoreDocOption::None;
    }

    args.mustCheckExistenceForInsertOperations =
        driver->getUpdateExecutor()->getCheckExistenceForDiffInsertOperations();

    args.retryableWrite = write_stage_common::isRetryableWrite(opCtx);

    BSONObj newObj;
    bool indexesAffected = false;
    if (inPlace) {
        if (!request->getIsExplain()) {
            if (isUserInitiatedWrite) {
                ShardingChecksForUpdate scfu(
                    collection,
                    request->getAllowShardKeyUpdatesWithoutFullShardKeyInQuery(),
                    request->isMulti(),
                    cq);
                scfu.checkUpdateChangesShardKeyFields(opCtx, doc, boost::none /* newObj */, oldObj);
            }

            auto diff = update_oplog_entry::extractDiffFromOplogEntry(logObj);
            WriteUnitOfWork wunit(opCtx);
            newObj = uassertStatusOK(collection_internal::updateDocumentWithDamages(
                opCtx,
                collection.getCollectionPtr(),
                rid,
                oldObj,
                source,
                damages,
                diff.has_value() ? &*diff : collection_internal::kUpdateAllIndexes,
                &indexesAffected,
                &CurOp::get(opCtx)->debug(),
                &args,
                cursor));

            tassert(11533702,
                    "Old and new snapshot ids must not change after update",
                    oldObj.snapshotId() ==
                        shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId());
            wunit.commit();
        } else {
            newObj = oldObjValue;
        }
    } else {
        // The updates were not in place. Apply them through the file manager.
        newObj = doc.getObject();
        if (!DocumentValidationSettings::get(opCtx).isInternalValidationDisabled()) {
            uassert(ErrorCodes::BSONObjectTooLarge,
                    str::stream() << "Resulting document after update is larger than "
                                  << BSONObjMaxUserSize,
                    newObj.objsize() <= BSONObjMaxUserSize);
        }

        if (!request->getIsExplain()) {
            if (isUserInitiatedWrite) {
                ShardingChecksForUpdate scfu(
                    collection,
                    request->getAllowShardKeyUpdatesWithoutFullShardKeyInQuery(),
                    request->isMulti(),
                    cq);
                scfu.checkUpdateChangesShardKeyFields(opCtx, doc, newObj, oldObj);
            }

            auto diff = update_oplog_entry::extractDiffFromOplogEntry(logObj);
            WriteUnitOfWork wunit(opCtx);
            collection_internal::updateDocument(
                opCtx,
                collection.getCollectionPtr(),
                rid,
                oldObj,
                newObj,
                diff.has_value() ? &*diff : collection_internal::kUpdateAllIndexes,
                &indexesAffected,
                &CurOp::get(opCtx)->debug(),
                &args);
            tassert(11533701,
                    "Old and new snapshot ids must not change after update",
                    oldObj.snapshotId() ==
                        shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId());
            wunit.commit();
        }
    }

    // If the document is indexed and the mod changes an indexed value, we might see it again.
    // For an example, see the comment above near declaration of '_updatedRecordIds' in
    // UpdateStage.
    //
    // This must be done after the wunit commits so we are sure we won't be rolling back.
    if (updatedRecordIds && indexesAffected) {
        updatedRecordIds->insert(rid);
    }

    return {newObj, docWasModified};
}

UpdateResult parseAndTransformOplogUpdate(OperationContext* opCtx,
                                          const CollectionAcquisition& coll,
                                          const Snapshotted<BSONObj>& oldObj,
                                          const UpdateRequest& request,
                                          const RecordId& rid,
                                          const SeekableRecordCursor* cursor) {
    // TODO SERVER-118695 Support upsert requests
    tassert(7834901, "This helper cannot be used to serve upsert requests.", !request.isUpsert());
    tassert(7834900,
            "This helper cannot serve update requests that have a collation set.",
            request.getCollation().isEmpty());
    auto [collatorToUse, expCtxCollationMatchesDefault] =
        resolveCollator(opCtx, request.getCollation(), coll.getCollectionPtr());

    // Create an ExpressionContext. This will allow us to create a ParsedUpdate and then a
    // CanonicalUpdate to obtain an UpdateDriver.
    auto expCtx = ExpressionContextBuilder{}
                      .fromRequest(opCtx, request)
                      .collator(std::move(collatorToUse))
                      .collationMatchesDefault(expCtxCollationMatchesDefault)
                      .build();

    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx,
        &request,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx, &request.getNsString())));

    auto canonicalUpdate = uassertStatusOK(
        CanonicalUpdate::make(expCtx, std::move(parsedUpdate), coll.getCollectionPtr()));

    mutablebson::Document doc{};
    auto* driver = canonicalUpdate->getDriver();
    // This helper should only be used for oplog application.
    constexpr bool isUserInitiatedWrite = false;
    auto [newObj, docWasModified] = transformDocument(opCtx,
                                                      coll,
                                                      oldObj,
                                                      doc,
                                                      isUserInitiatedWrite,
                                                      nullptr, /* cq */
                                                      rid,
                                                      driver,
                                                      &request,
                                                      false,   /* shouldWriteToOrphan */
                                                      nullptr, /* updatedRecordIds*/
                                                      cursor);

    // If we called this function, then we must have matched the targeted document.
    unsigned long long numMatched = 1ULL;
    UpdateResult ur{docWasModified,
                    driver->type() == UpdateDriver::UpdateType::kOperator, /* modifiers */
                    docWasModified ? 1ULL : 0ULL,                          /* numDocsModified */
                    numMatched,
                    BSONObj::kEmptyObject, /* upsertedObject */
                    driver->containsDotsAndDollarsField()};

    if (request.shouldReturnNewDocs()) {
        ur.requestedDocImage = newObj.getOwned();
    } else if (request.shouldReturnOldDocs()) {
        ur.requestedDocImage = oldObj.value().getOwned();
    }

    return ur;
}

}  // namespace update
}  // namespace mongo
