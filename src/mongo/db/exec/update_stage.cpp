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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/update_stage.h"

#include <algorithm>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeUpsertPerformsInsert);
MONGO_FAIL_POINT_DEFINE(hangBeforeThrowWouldChangeOwningShard);

using std::string;
using std::unique_ptr;
using std::vector;

namespace mb = mutablebson;

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

Status ensureIdFieldIsFirst(mb::Document* doc) {
    mb::Element idElem = mb::findFirstChildNamed(doc->root(), idFieldName);

    if (!idElem.ok()) {
        return {ErrorCodes::InvalidIdField, "_id field is missing"};
    }

    if (idElem.leftSibling().ok()) {
        // Move '_id' to be the first element
        Status s = idElem.remove();
        if (!s.isOK())
            return s;
        s = doc->root().pushFront(idElem);
        if (!s.isOK())
            return s;
    }

    return Status::OK();
}

void addObjectIDIdField(mb::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    if (!idElem.ok())
        uasserted(17268, "Could not create new ObjectId '_id' field.");

    uassertStatusOK(doc->root().pushFront(idElem));
}

/**
 * Uasserts if any of the paths in 'requiredPaths' are not present in 'document', or if they are
 * arrays or array descendants.
 */
void assertRequiredPathsPresent(const mb::Document& document, const FieldRefSet& requiredPaths) {
    for (const auto& path : requiredPaths) {
        auto elem = document.root();
        for (size_t i = 0; i < (*path).numParts(); ++i) {
            elem = elem[(*path).getPart(i)];
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "After applying the update, the new document was missing the "
                                     "required field '"
                                  << (*path).dottedField() << "'",
                    elem.ok());
            uassert(
                ErrorCodes::NotSingleValueField,
                str::stream() << "After applying the update to the document, the required field '"
                              << (*path).dottedField()
                              << "' was found to be an array or array descendant.",
                elem.getType() != BSONType::Array);
        }
    }
}

/**
 * Returns true if we should throw a WriteConflictException in order to retry the operation in the
 * case of a conflict. Returns false if we should skip the document and keep going.
 */
bool shouldRestartUpdateIfNoLongerMatches(const UpdateStageParams& params) {
    // When we're doing a findAndModify with a sort, the sort will have a limit of 1, so it will not
    // produce any more results even if there is another matching document. Throw a WCE here so that
    // these operations get another chance to find a matching document. The findAndModify command
    // should automatically retry if it gets a WCE.
    return params.request->shouldReturnAnyDocs() && !params.request->getSort().isEmpty();
};

CollectionUpdateArgs::StoreDocOption getStoreDocMode(const UpdateRequest& updateRequest) {
    if (updateRequest.shouldReturnNewDocs()) {
        return CollectionUpdateArgs::StoreDocOption::PostImage;
    }

    if (updateRequest.shouldReturnOldDocs()) {
        return CollectionUpdateArgs::StoreDocOption::PreImage;
    }

    invariant(!updateRequest.shouldReturnAnyDocs());
    return CollectionUpdateArgs::StoreDocOption::None;
}
}  // namespace

const char* UpdateStage::kStageType = "UPDATE";

const UpdateStats UpdateStage::kEmptyUpdateStats;

UpdateStage::UpdateStage(OperationContext* opCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : RequiresMutableCollectionStage(kStageType, opCtx, collection),
      _params(params),
      _ws(ws),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new RecordIdSet() : nullptr),
      _doc(params.driver->getDocument()) {
    _children.emplace_back(child);

    // Should the modifiers validate their embedded docs via storage_validation::storageValid()?
    // Only user updates should be checked. Any system or replication stuff should pass through.
    // Config db docs also do not get checked.
    const auto request = _params.request;
    _enforceOkForStorage =
        !(request->isFromOplogApplication() || request->getNamespaceString().isConfigDB() ||
          request->isFromMigration());

    // We should only check for an update to the shard key if the update is coming from a user and
    // the request is versioned.
    _shouldCheckForShardKeyUpdate =
        !(request->isFromOplogApplication() || request->getNamespaceString().isConfigDB() ||
          request->isFromMigration()) &&
        OperationShardingState::isOperationVersioned(opCtx);

    _specificStats.isModUpdate = params.driver->type() == UpdateDriver::UpdateType::kOperator;
}

BSONObj UpdateStage::transformAndUpdate(const Snapshotted<BSONObj>& oldObj, RecordId& recordId) {
    const UpdateRequest* request = _params.request;
    UpdateDriver* driver = _params.driver;
    CanonicalQuery* cq = _params.canonicalQuery;

    // If asked to return new doc, default to the oldObj, in case nothing changes.
    BSONObj newObj = oldObj.value();

    // Ask the driver to apply the mods. It may be that the driver can apply those "in
    // place", that is, some values of the old document just get adjusted without any
    // change to the binary layout on the bson layer. It may be that a whole new document
    // is needed to accomodate the new bson layout of the resulting document. In any event,
    // only enable in-place mutations if the underlying storage engine offers support for
    // writing damage events.
    _doc.reset(oldObj.value(),
               (collection()->updateWithDamagesSupported()
                    ? mutablebson::Document::kInPlaceEnabled
                    : mutablebson::Document::kInPlaceDisabled));

    BSONObj logObj;

    bool docWasModified = false;

    auto* const css = CollectionShardingState::get(getOpCtx(), collection()->ns());
    auto metadata = css->getCurrentMetadata();
    Status status = Status::OK();
    const bool validateForStorage = getOpCtx()->writesAreReplicated() && _enforceOkForStorage;
    const bool isInsert = false;
    FieldRefSet immutablePaths;
    if (getOpCtx()->writesAreReplicated() && !request->isFromMigration()) {
        if (metadata->isSharded() && !OperationShardingState::isOperationVersioned(getOpCtx())) {
            auto& immutablePathsVector = metadata->getKeyPatternFields();
            immutablePaths.fillFrom(
                transitional_tools_do_not_use::unspool_vector(immutablePathsVector));
        }
        immutablePaths.keepShortest(&idFieldRef);
    }
    if (!driver->needMatchDetails()) {
        // If we don't need match details, avoid doing the rematch
        status = driver->update(StringData(),
                                &_doc,
                                validateForStorage,
                                immutablePaths,
                                isInsert,
                                &logObj,
                                &docWasModified);
    } else {
        // If there was a matched field, obtain it.
        MatchDetails matchDetails;
        matchDetails.requestElemMatchKey();

        dassert(cq);
        verify(cq->root()->matchesBSON(oldObj.value(), &matchDetails));

        string matchedField;
        if (matchDetails.hasElemMatchKey())
            matchedField = matchDetails.elemMatchKey();

        status = driver->update(matchedField,
                                &_doc,
                                validateForStorage,
                                immutablePaths,
                                isInsert,
                                &logObj,
                                &docWasModified);
    }

    if (!status.isOK()) {
        uasserted(16837, status.reason());
    }

    // Skip adding _id field if the collection is capped (since capped collection documents can
    // neither grow nor shrink).
    const auto createIdField = !collection()->isCapped();

    // Ensure if _id exists it is first
    status = ensureIdFieldIsFirst(&_doc);
    if (status.code() == ErrorCodes::InvalidIdField) {
        // Create ObjectId _id field if we are doing that
        if (createIdField) {
            addObjectIDIdField(&_doc);
        }
    } else {
        uassertStatusOK(status);
    }

    // See if the changes were applied in place
    const char* source = nullptr;
    const bool inPlace = _doc.getInPlaceUpdates(&_damages, &source);

    if (inPlace && _damages.empty()) {
        // An interesting edge case. A modifier didn't notice that it was really a no-op
        // during its 'prepare' phase. That represents a missed optimization, but we still
        // shouldn't do any real work. Toggle 'docWasModified' to 'false'.
        //
        // Currently, an example of this is '{ $push : { x : {$each: [], $sort: 1} } }' when the 'x'
        // array exists and is already sorted.
        docWasModified = false;
    }

    if (docWasModified) {

        // Prepare to write back the modified document

        RecordId newRecordId;
        CollectionUpdateArgs args;

        if (!request->isExplain()) {
            args.stmtId = request->getStmtId();
            args.update = logObj;
            args.criteria = metadata->extractDocumentKey(newObj);
            uassert(16980,
                    "Multi-update operations require all documents to have an '_id' field",
                    !request->isMulti() || args.criteria.hasField("_id"_sd));
            args.fromMigrate = request->isFromMigration();
            args.storeDocOption = getStoreDocMode(*request);
            if (args.storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                args.preImageDoc = oldObj.value().getOwned();
            }
        }

        if (inPlace) {
            if (!request->isExplain()) {
                newObj = oldObj.value();
                const RecordData oldRec(oldObj.value().objdata(), oldObj.value().objsize());

                Snapshotted<RecordData> snap(oldObj.snapshotId(), oldRec);

                if (metadata->isSharded() && _shouldCheckForShardKeyUpdate) {
                    bool changesShardKeyOnSameNode =
                        checkUpdateChangesShardKeyFields(metadata, oldObj);
                    if (changesShardKeyOnSameNode && !args.preImageDoc) {
                        args.preImageDoc = oldObj.value().getOwned();
                    }
                }

                WriteUnitOfWork wunit(getOpCtx());
                StatusWith<RecordData> newRecStatus = collection()->updateDocumentWithDamages(
                    getOpCtx(), recordId, std::move(snap), source, _damages, &args);
                invariant(oldObj.snapshotId() == getOpCtx()->recoveryUnit()->getSnapshotId());
                wunit.commit();

                newObj = uassertStatusOK(std::move(newRecStatus)).releaseToBson();
            }

            newRecordId = recordId;
        } else {
            // The updates were not in place. Apply them through the file manager.

            newObj = _doc.getObject();
            uassert(17419,
                    str::stream() << "Resulting document after update is larger than "
                                  << BSONObjMaxUserSize,
                    newObj.objsize() <= BSONObjMaxUserSize);

            if (!request->isExplain()) {
                if (metadata->isSharded() && _shouldCheckForShardKeyUpdate) {
                    bool changesShardKeyOnSameNode =
                        checkUpdateChangesShardKeyFields(metadata, oldObj);
                    if (changesShardKeyOnSameNode && !args.preImageDoc) {
                        args.preImageDoc = oldObj.value().getOwned();
                    }
                }

                WriteUnitOfWork wunit(getOpCtx());
                newRecordId = collection()->updateDocument(getOpCtx(),
                                                           recordId,
                                                           oldObj,
                                                           newObj,
                                                           driver->modsAffectIndices(),
                                                           _params.opDebug,
                                                           &args);
                invariant(oldObj.snapshotId() == getOpCtx()->recoveryUnit()->getSnapshotId());
                wunit.commit();
            }
        }

        // If the document moved, we might see it again in a collection scan (maybe it's
        // a document after our current document).
        //
        // If the document is indexed and the mod changes an indexed value, we might see
        // it again.  For an example, see the comment above near declaration of
        // updatedRecordIds.
        //
        // This must be done after the wunit commits so we are sure we won't be rolling back.
        if (_updatedRecordIds && (newRecordId != recordId || driver->modsAffectIndices())) {
            _updatedRecordIds->insert(newRecordId);
        }
    }

    // Only record doc modifications if they wrote (exclude no-ops). Explains get
    // recorded as if they wrote.
    if (docWasModified || request->isExplain()) {
        _specificStats.nModified++;
    }

    return newObj;
}

BSONObj UpdateStage::applyUpdateOpsForInsert(OperationContext* opCtx,
                                             const CanonicalQuery* cq,
                                             const BSONObj& query,
                                             UpdateDriver* driver,
                                             mutablebson::Document* doc,
                                             bool isInternalRequest,
                                             const NamespaceString& ns,
                                             bool enforceOkForStorage,
                                             UpdateStats* stats) {
    // Since this is an insert (no docs found and upsert:true), we will be logging it
    // as an insert in the oplog. We don't need the driver's help to build the
    // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
    // Some mods may only work in that context (e.g. $setOnInsert).
    driver->setLogOp(false);

    auto* const css = CollectionShardingState::get(opCtx, ns);
    auto metadata = css->getCurrentMetadata();

    FieldRefSet immutablePaths;
    if (metadata->isSharded() && !OperationShardingState::isOperationVersioned(opCtx)) {
        auto& immutablePathsVector = metadata->getKeyPatternFields();
        immutablePaths.fillFrom(
            transitional_tools_do_not_use::unspool_vector(immutablePathsVector));
    }
    immutablePaths.keepShortest(&idFieldRef);

    if (cq) {
        FieldRefSet requiredPaths;
        if (metadata->isSharded()) {
            const auto& shardKeyPathsVector = metadata->getKeyPatternFields();
            requiredPaths.fillFrom(
                transitional_tools_do_not_use::unspool_vector(shardKeyPathsVector));
        }
        requiredPaths.keepShortest(&idFieldRef);
        uassertStatusOK(driver->populateDocumentWithQueryFields(*cq, requiredPaths, *doc));
    } else {
        fassert(17354, CanonicalQuery::isSimpleIdQuery(query));
        BSONElement idElt = query[idFieldName];
        fassert(17352, doc->root().appendElement(idElt));
    }

    // Apply the update modifications here. Do not validate for storage, since we will validate the
    // entire document after the update. However, we ensure that no immutable fields are updated.
    const bool validateForStorage = false;
    const bool isInsert = true;
    if (isInternalRequest) {
        immutablePaths.clear();
    }
    Status updateStatus =
        driver->update(StringData(), doc, validateForStorage, immutablePaths, isInsert);
    if (!updateStatus.isOK()) {
        uasserted(16836, updateStatus.reason());
    }

    // Ensure _id exists and is first
    auto idAndFirstStatus = ensureIdFieldIsFirst(doc);
    if (idAndFirstStatus.code() == ErrorCodes::InvalidIdField) {  // _id field is missing
        addObjectIDIdField(doc);
    } else {
        uassertStatusOK(idAndFirstStatus);
    }

    // Validate that the object replacement or modifiers resulted in a document
    // that contains all the required keys and can be stored if it isn't coming
    // from a migration or via replication.
    if (!isInternalRequest) {
        if (enforceOkForStorage) {
            storage_validation::storageValid(*doc);
        }
        FieldRefSet requiredPaths;
        if (metadata->isSharded()) {
            const auto& shardKeyPathsVector = metadata->getKeyPatternFields();
            requiredPaths.fillFrom(
                transitional_tools_do_not_use::unspool_vector(shardKeyPathsVector));
        }
        requiredPaths.keepShortest(&idFieldRef);
        assertRequiredPathsPresent(*doc, requiredPaths);
    }

    BSONObj newObj = doc->getObject();
    if (newObj.objsize() > BSONObjMaxUserSize) {
        uasserted(17420,
                  str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize);
    }

    return newObj;
}

bool UpdateStage::matchContainsOnlyAndedEqualityNodes(const MatchExpression& root) {
    if (root.matchType() == MatchExpression::EQ) {
        return true;
    }

    if (root.matchType() == MatchExpression::AND) {
        for (size_t i = 0; i < root.numChildren(); ++i) {
            if (root.getChild(i)->matchType() != MatchExpression::EQ) {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool UpdateStage::shouldRetryDuplicateKeyException(const ParsedUpdate& parsedUpdate,
                                                   const DuplicateKeyErrorInfo& errorInfo) {
    invariant(parsedUpdate.hasParsedQuery());

    const auto updateRequest = parsedUpdate.getRequest();

    // In order to be retryable, the update must be an upsert with multi:false.
    if (!updateRequest->isUpsert() || updateRequest->isMulti()) {
        return false;
    }

    auto matchExpr = parsedUpdate.getParsedQuery()->root();
    invariant(matchExpr);

    // In order to be retryable, the update query must contain no expressions other than AND and EQ.
    if (!matchContainsOnlyAndedEqualityNodes(*matchExpr)) {
        return false;
    }

    // In order to be retryable, the update equality field paths must be identical to the unique
    // index key field paths. Also, the values that triggered the DuplicateKey error must match the
    // values used in the upsert query predicate.
    pathsupport::EqualityMatches equalities;
    auto status = pathsupport::extractEqualityMatches(*matchExpr, &equalities);
    if (!status.isOK()) {
        return false;
    }

    auto keyPattern = errorInfo.getKeyPattern();
    if (equalities.size() != static_cast<size_t>(keyPattern.nFields())) {
        return false;
    }

    auto keyValue = errorInfo.getDuplicatedKeyValue();

    BSONObjIterator keyPatternIter(keyPattern);
    BSONObjIterator keyValueIter(keyValue);
    while (keyPatternIter.more() && keyValueIter.more()) {
        auto keyPatternElem = keyPatternIter.next();
        auto keyValueElem = keyValueIter.next();

        auto keyName = keyPatternElem.fieldNameStringData();
        if (!equalities.count(keyName)) {
            return false;
        }

        // Comparison which obeys field ordering but ignores field name.
        BSONElementComparator cmp{BSONElementComparator::FieldNamesMode::kIgnore, nullptr};
        if (cmp.evaluate(equalities[keyName]->getData() != keyValueElem)) {
            return false;
        }
    }
    invariant(!keyPatternIter.more());
    invariant(!keyValueIter.more());

    return true;
}

void UpdateStage::doInsert() {
    _specificStats.inserted = true;

    const UpdateRequest* request = _params.request;
    bool isInternalRequest = !getOpCtx()->writesAreReplicated() || request->isFromMigration();

    // Reset the document we will be writing to.
    _doc.reset();

    BSONObj newObj = applyUpdateOpsForInsert(getOpCtx(),
                                             _params.canonicalQuery,
                                             request->getQuery(),
                                             _params.driver,
                                             &_doc,
                                             isInternalRequest,
                                             request->getNamespaceString(),
                                             _enforceOkForStorage,
                                             &_specificStats);

    _specificStats.objInserted = newObj;

    // If this is an explain, bail out now without doing the insert.
    if (request->isExplain()) {
        return;
    }

    // If in FCV 4.2 and this collection is sharded, check if the doc we plan to insert belongs to
    // this shard. MongoS uses the query field to target a shard, and it is possible the shard key
    // fields in the 'q' field belong to this shard, but those in the 'u' field do not. In this case
    // we need to throw so that MongoS can target the insert to the correct shard.
    if (_shouldCheckForShardKeyUpdate) {
        auto* const css = CollectionShardingState::get(getOpCtx(), collection()->ns());
        const auto& metadata = css->getCurrentMetadata();

        if (metadata->isSharded()) {
            const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
            auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newObj);

            if (!metadata->keyBelongsToMe(newShardKey)) {
                // An attempt to upsert a document with a shard key value that belongs on another
                // shard must either be a retryable write or inside a transaction.
                uassert(ErrorCodes::IllegalOperation,
                        "The upsert document could not be inserted onto the shard targeted by the "
                        "query, since its shard key belongs on a different shard. Cross-shard "
                        "upserts are only allowed when running in a transaction or with "
                        "retryWrites: true.",
                        getOpCtx()->getTxnNumber());
                uasserted(
                    WouldChangeOwningShardInfo(request->getQuery(), newObj, true /* upsert */),
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
        uassertStatusOK(collection()->insertDocument(getOpCtx(),
                                                     InsertStatement(request->getStmtId(), newObj),
                                                     _params.opDebug,
                                                     request->isFromMigration()));

        // Technically, we should save/restore state here, but since we are going to return
        // immediately after, it would just be wasted work.
        wunit.commit();
    });
}

bool UpdateStage::doneUpdating() {
    // We're done updating if either the child has no more results to give us, or we've
    // already gotten a result back and we're not a multi-update.
    return _idRetrying == WorkingSet::INVALID_ID && _idReturning == WorkingSet::INVALID_ID &&
        (child()->isEOF() || (_specificStats.nMatched > 0 && !_params.request->isMulti()));
}

bool UpdateStage::needInsert() {
    // We need to insert if
    //  1) we haven't inserted already,
    //  2) the child stage returned zero matches, and
    //  3) the user asked for an upsert.
    return !_specificStats.inserted && _specificStats.nMatched == 0 && _params.request->isUpsert();
}

bool UpdateStage::isEOF() {
    return doneUpdating() && !needInsert();
}

PlanStage::StageState UpdateStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (doneUpdating()) {
        // Even if we're done updating, we may have some inserting left to do.
        if (needInsert()) {

            doInsert();

            invariant(isEOF());
            if (_params.request->shouldReturnNewDocs()) {
                // Want to return the document we just inserted, create it as a WorkingSetMember
                // so that we can return it.
                BSONObj newObj = _specificStats.objInserted;
                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->resetDocument(getOpCtx()->recoveryUnit()->getSnapshotId(),
                                      newObj.getOwned());
                member->transitionToOwnedObj();
                return PlanStage::ADVANCED;
            }
        }

        // At this point either we're done updating and there was no insert to do,
        // or we're done updating and we're done inserting. Either way, we're EOF.
        invariant(isEOF());
        return PlanStage::IS_EOF;
    }

    // It is possible that after an update was applied, a WriteConflictException
    // occurred and prevented us from returning ADVANCED with the requested version
    // of the document.
    if (_idReturning != WorkingSet::INVALID_ID) {
        // We should only get here if we were trying to return something before.
        invariant(_params.request->shouldReturnAnyDocs());

        WorkingSetMember* member = _ws->get(_idReturning);
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        *out = _idReturning;
        _idReturning = WorkingSet::INVALID_ID;
        return PlanStage::ADVANCED;
    }

    // Either retry the last WSM we worked on or get a new one from our child.
    WorkingSetID id;
    StageState status;
    if (_idRetrying == WorkingSet::INVALID_ID) {
        status = child()->work(&id);
    } else {
        status = ADVANCED;
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    if (PlanStage::ADVANCED == status) {
        // Need to get these things from the result returned by the child.
        RecordId recordId;

        WorkingSetMember* member = _ws->get(id);

        // We want to free this member when we return, unless we need to retry updating or returning
        // it.
        auto memberFreer = makeGuard([&] { _ws->free(id); });

        invariant(member->hasRecordId());
        recordId = member->recordId;

        // Updates can't have projections. This means that covering analysis will always add
        // a fetch. We should always get fetched data, and never just key data.
        invariant(member->hasObj());

        // We fill this with the new RecordIds of moved doc so we don't double-update.
        if (_updatedRecordIds && _updatedRecordIds->count(recordId) > 0) {
            // Found a RecordId that refers to a document we had already updated. Note that
            // we can never remove from _updatedRecordIds because updates by other clients
            // could cause us to encounter a document again later.
            return PlanStage::NEED_TIME;
        }

        bool docStillMatches;
        try {
            docStillMatches = write_stage_common::ensureStillMatches(
                collection(), getOpCtx(), _ws, id, _params.canonicalQuery);
        } catch (const WriteConflictException&) {
            // There was a problem trying to detect if the document still exists, so retry.
            memberFreer.dismiss();
            return prepareToRetryWSM(id, out);
        }

        if (!docStillMatches) {
            // Either the document has been deleted, or it has been updated such that it no longer
            // matches the predicate.
            if (shouldRestartUpdateIfNoLongerMatches(_params)) {
                throw WriteConflictException();
            }
            return PlanStage::NEED_TIME;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState()
        // is allowed to free the memory.
        member->makeObjOwnedIfNeeded();

        // Save state before making changes.
        try {
            child()->saveState();
        } catch (const WriteConflictException&) {
            std::terminate();
        }

        // If we care about the pre-updated version of the doc, save it out here.
        BSONObj oldObj;
        SnapshotId oldSnapshot = member->doc.snapshotId();
        if (_params.request->shouldReturnOldDocs()) {
            oldObj = member->doc.value().toBson().getOwned();
        }

        BSONObj newObj;
        try {
            // Do the update, get us the new version of the doc.
            newObj = transformAndUpdate({oldSnapshot, member->doc.value().toBson()}, recordId);
        } catch (const WriteConflictException&) {
            memberFreer.dismiss();  // Keep this member around so we can retry updating it.
            return prepareToRetryWSM(id, out);
        }

        // Set member's obj to be the doc we want to return.
        if (_params.request->shouldReturnAnyDocs()) {
            if (_params.request->shouldReturnNewDocs()) {
                member->resetDocument(getOpCtx()->recoveryUnit()->getSnapshotId(),
                                      newObj.getOwned());
            } else {
                invariant(_params.request->shouldReturnOldDocs());
                member->resetDocument(oldSnapshot, oldObj);
            }
            member->recordId = RecordId();
            member->transitionToOwnedObj();
        }

        // This should be after transformAndUpdate to make sure we actually updated this doc.
        ++_specificStats.nMatched;

        // Restore state after modification

        // As restoreState may restore (recreate) cursors, make sure to restore the
        // state outside of the WritUnitOfWork.
        try {
            child()->restoreState();
        } catch (const WriteConflictException&) {
            // Note we don't need to retry updating anything in this case since the update
            // already was committed. However, we still need to return the updated document
            // (if it was requested).
            if (_params.request->shouldReturnAnyDocs()) {
                // member->obj should refer to the document we want to return.
                invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

                _idReturning = id;
                // Keep this member around so that we can return it on the next work() call.
                memberFreer.dismiss();
            }
            *out = WorkingSet::INVALID_ID;
            return NEED_YIELD;
        }

        if (_params.request->shouldReturnAnyDocs()) {
            // member->obj should refer to the document we want to return.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            memberFreer.dismiss();  // Keep this member around so we can return it.
            *out = id;
            return PlanStage::ADVANCED;
        }

        return PlanStage::NEED_TIME;
    } else if (PlanStage::IS_EOF == status) {
        // The child is out of results, but we might not be done yet because we still might
        // have to do an insert.
        return PlanStage::NEED_TIME;
    } else if (PlanStage::FAILURE == status) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it failed, in which case
        // 'id' is valid.  If ID is invalid, we create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            const std::string errmsg = "update stage failed to read in results from child";
            *out = WorkingSetCommon::allocateStatusMember(
                _ws, Status(ErrorCodes::InternalError, errmsg));
            return PlanStage::FAILURE;
        }
        return status;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

void UpdateStage::doRestoreStateRequiresCollection() {
    const UpdateRequest& request = *_params.request;
    const NamespaceString& nsString(request.getNamespaceString());

    // We may have stepped down during the yield.
    bool userInitiatedWritesAndNotPrimary = getOpCtx()->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(getOpCtx())->canAcceptWritesFor(getOpCtx(), nsString);

    if (userInitiatedWritesAndNotPrimary) {
        uasserted(ErrorCodes::PrimarySteppedDown,
                  str::stream() << "Demoted from primary while performing update on "
                                << nsString.ns());
    }

    // The set of indices may have changed during yield. Make sure that the update driver has up to
    // date index information.
    const auto& updateIndexData = CollectionQueryInfo::get(collection()).getIndexKeys(getOpCtx());
    _params.driver->refreshIndexKeys(&updateIndexData);
}

unique_ptr<PlanStageStats> UpdateStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_UPDATE);
    ret->specific = std::make_unique<UpdateStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* UpdateStage::getSpecificStats() const {
    return &_specificStats;
}

const UpdateStats* UpdateStage::getUpdateStats(const PlanExecutor* exec) {
    invariant(exec->getRootStage()->isEOF());

    // If we're updating a non-existent collection, then the delete plan may have an EOF as the root
    // stage.
    if (exec->getRootStage()->stageType() == STAGE_EOF) {
        return &kEmptyUpdateStats;
    }

    // If the collection exists, then we expect the root of the plan tree to either be an update
    // stage, or (for findAndModify) a projection stage wrapping an update stage.
    switch (exec->getRootStage()->stageType()) {
        case StageType::STAGE_PROJECTION_DEFAULT:
        case StageType::STAGE_PROJECTION_COVERED:
        case StageType::STAGE_PROJECTION_SIMPLE: {
            invariant(exec->getRootStage()->getChildren().size() == 1U);
            invariant(StageType::STAGE_UPDATE == exec->getRootStage()->child()->stageType());
            const SpecificStats* stats = exec->getRootStage()->child()->getSpecificStats();
            return static_cast<const UpdateStats*>(stats);
        }
        default:
            invariant(StageType::STAGE_UPDATE == exec->getRootStage()->stageType());
            return static_cast<const UpdateStats*>(exec->getRootStage()->getSpecificStats());
    }
}

void UpdateStage::recordUpdateStatsInOpDebug(const UpdateStats* updateStats, OpDebug* opDebug) {
    invariant(opDebug);
    opDebug->additiveMetrics.nMatched = updateStats->nMatched;
    opDebug->additiveMetrics.nModified = updateStats->nModified;
    opDebug->upsert = updateStats->inserted;
}

UpdateResult UpdateStage::makeUpdateResult(const UpdateStats* updateStats) {
    return UpdateResult(updateStats->nMatched > 0 /* Did we update at least one obj? */,
                        updateStats->isModUpdate /* Is this a $mod update? */,
                        updateStats->nModified /* number of modified docs, no no-ops */,
                        updateStats->nMatched /* # of docs matched/updated, even no-ops */,
                        updateStats->objInserted);
};

PlanStage::StageState UpdateStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

bool UpdateStage::checkUpdateChangesShardKeyFields(ScopedCollectionMetadata metadata,
                                                   const Snapshotted<BSONObj>& oldObj) {
    auto newObj = _doc.getObject();
    const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
    auto oldShardKey = shardKeyPattern.extractShardKeyFromDoc(oldObj.value());
    auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newObj);

    // If the shard key fields remain unchanged by this update or if this document is an orphan and
    // so does not belong to this shard, we can skip the rest of the checks.
    if ((newShardKey.woCompare(oldShardKey) == 0) || !metadata->keyBelongsToMe(oldShardKey)) {
        return false;
    }

    FieldRefSet shardKeyPaths;
    const auto& shardKeyPathsVector = metadata->getKeyPatternFields();
    shardKeyPaths.fillFrom(transitional_tools_do_not_use::unspool_vector(shardKeyPathsVector));

    // Assert that the updated doc has all shard key fields and none are arrays or array
    // descendants.
    assertRequiredPathsPresent(_doc, shardKeyPaths);

    // We do not allow modifying shard key value without specifying the full shard key in the query.
    // If the query is a simple equality match on _id, then '_params.canonicalQuery' will be null.
    // But if we are here, we already know that the shard key is not _id, since we have an assertion
    // earlier for requests that try to modify the immutable _id field. So it is safe to uassert if
    // '_params.canonicalQuery' is null OR if the query does not include equality matches on all
    // shard key fields.
    pathsupport::EqualityMatches equalities;
    uassert(31025,
            "Shard key update is not allowed without specifying the full shard key in the query",
            _params.canonicalQuery &&
                pathsupport::extractFullEqualityMatches(
                    *(_params.canonicalQuery->root()), shardKeyPaths, &equalities)
                    .isOK() &&
                equalities.size() == shardKeyPathsVector.size());

    // We do not allow updates to the shard key when 'multi' is true.
    uassert(ErrorCodes::InvalidOptions,
            "Multi-update operations are not allowed when updating the shard key field.",
            !_params.request->isMulti());

    // If this node is a replica set primary node, an attempted update to the shard key value must
    // either be a retryable write or inside a transaction.
    // If this node is a replica set secondary node, we can skip validation.
    uassert(ErrorCodes::IllegalOperation,
            "Must run update to shard key field in a multi-statement transaction or with "
            "retryWrites: true.",
            getOpCtx()->getTxnNumber() || !getOpCtx()->writesAreReplicated());

    if (!metadata->keyBelongsToMe(newShardKey)) {
        if (MONGO_unlikely(hangBeforeThrowWouldChangeOwningShard.shouldFail())) {
            log() << "Hit hangBeforeThrowWouldChangeOwningShard failpoint";
            hangBeforeThrowWouldChangeOwningShard.pauseWhileSet(getOpCtx());
        }

        uasserted(WouldChangeOwningShardInfo(oldObj.value(), newObj, false /* upsert */),
                  "This update would cause the doc to change owning shards");
    }

    // We passed all checks, so we will return that this update changes the shard key field, and
    // the updated document will remain on the same node.
    return true;
}

}  // namespace mongo
