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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/update_stage.h"

#include <algorithm>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
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
#include "mongo/logv2/log.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeThrowWouldChangeOwningShard);

using std::string;
using std::unique_ptr;
using std::vector;

namespace mb = mutablebson;

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

void addObjectIDIdField(mb::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    uassert(17268, "Could not create new ObjectId '_id' field.", idElem.ok());
    uassertStatusOK(doc->root().pushFront(idElem));
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

// Public constructor.
UpdateStage::UpdateStage(ExpressionContext* expCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : UpdateStage(expCtx, params, ws, collection) {
    // We should never reach here if the request is an upsert.
    invariant(!_params.request->isUpsert());
    _children.emplace_back(child);
}

// Protected constructor.
UpdateStage::UpdateStage(ExpressionContext* expCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection)
    : RequiresMutableCollectionStage(kStageType, expCtx, collection),
      _params(params),
      _ws(ws),
      _doc(params.driver->getDocument()),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new RecordIdSet() : nullptr) {

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
        OperationShardingState::isOperationVersioned(expCtx->opCtx);

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

    auto* const css = CollectionShardingState::get(opCtx(), collection()->ns());
    const auto collDesc = css->getCollectionDescription_DEPRECATED();
    Status status = Status::OK();
    const bool validateForStorage = opCtx()->writesAreReplicated() && _enforceOkForStorage;
    const bool isInsert = false;
    FieldRefSet immutablePaths;
    if (opCtx()->writesAreReplicated() && !request->isFromMigration()) {
        if (collDesc.isSharded() && !OperationShardingState::isOperationVersioned(opCtx())) {
            immutablePaths.fillFrom(collDesc.getKeyPatternFields());
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

    // Ensure _id is first if it exists, and generate a new OID if appropriate.
    _ensureIdFieldIsFirst(&_doc, createIdField);

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
            args.criteria = collDesc.extractDocumentKey(newObj);
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

                if (collDesc.isSharded() && _shouldCheckForShardKeyUpdate) {
                    bool changesShardKeyOnSameNode =
                        checkUpdateChangesShardKeyFields(collDesc, oldObj);
                    if (changesShardKeyOnSameNode && !args.preImageDoc) {
                        args.preImageDoc = oldObj.value().getOwned();
                    }
                }

                WriteUnitOfWork wunit(opCtx());
                StatusWith<RecordData> newRecStatus = collection()->updateDocumentWithDamages(
                    opCtx(), recordId, std::move(snap), source, _damages, &args);
                invariant(oldObj.snapshotId() == opCtx()->recoveryUnit()->getSnapshotId());
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
                if (collDesc.isSharded() && _shouldCheckForShardKeyUpdate) {
                    bool changesShardKeyOnSameNode =
                        checkUpdateChangesShardKeyFields(collDesc, oldObj);
                    if (changesShardKeyOnSameNode && !args.preImageDoc) {
                        args.preImageDoc = oldObj.value().getOwned();
                    }
                }

                WriteUnitOfWork wunit(opCtx());
                newRecordId = collection()->updateDocument(opCtx(),
                                                           recordId,
                                                           oldObj,
                                                           newObj,
                                                           driver->modsAffectIndices(),
                                                           _params.opDebug,
                                                           &args);
                invariant(oldObj.snapshotId() == opCtx()->recoveryUnit()->getSnapshotId());
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

void UpdateStage::_assertPathsNotArray(const mb::Document& document, const FieldRefSet& paths) {
    for (const auto& path : paths) {
        auto elem = document.root();
        // If any path component does not exist, we stop checking for arrays along the path.
        for (size_t i = 0; elem.ok() && i < (*path).numParts(); ++i) {
            elem = elem[(*path).getPart(i)];
            uassert(ErrorCodes::NotSingleValueField,
                    str::stream() << "After applying the update to the document, the field '"
                                  << (*path).dottedField()
                                  << "' was found to be an array or array descendant.",
                    !elem.ok() || elem.getType() != BSONType::Array);
        }
    }
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

bool UpdateStage::isEOF() {
    // We're done updating if either the child has no more results to give us, or we've
    // already gotten a result back and we're not a multi-update.
    return _idRetrying == WorkingSet::INVALID_ID && _idReturning == WorkingSet::INVALID_ID &&
        (child()->isEOF() || (_specificStats.nMatched > 0 && !_params.request->isMulti()));
}

PlanStage::StageState UpdateStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
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
                collection(), opCtx(), _ws, id, _params.canonicalQuery);
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
                member->resetDocument(opCtx()->recoveryUnit()->getSnapshotId(), newObj.getOwned());
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
        // The child is out of results, and therefore so are we.
        return PlanStage::IS_EOF;
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

void UpdateStage::_ensureIdFieldIsFirst(mb::Document* doc, bool generateOIDIfMissing) {
    mb::Element idElem = mb::findFirstChildNamed(doc->root(), idFieldName);

    // If the document has no _id and the caller has requested that we generate one, do so.
    if (!idElem.ok() && generateOIDIfMissing) {
        addObjectIDIdField(doc);
    } else if (idElem.ok() && idElem.leftSibling().ok()) {
        // If the document does have an _id but it is not the first element, move it to the front.
        uassertStatusOK(idElem.remove());
        uassertStatusOK(doc->root().pushFront(idElem));
    }
}

void UpdateStage::doRestoreStateRequiresCollection() {
    const UpdateRequest& request = *_params.request;
    const NamespaceString& nsString(request.getNamespaceString());

    // We may have stepped down during the yield.
    bool userInitiatedWritesAndNotPrimary = opCtx()->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), nsString);

    if (userInitiatedWritesAndNotPrimary) {
        uasserted(ErrorCodes::PrimarySteppedDown,
                  str::stream() << "Demoted from primary while performing update on "
                                << nsString.ns());
    }

    // The set of indices may have changed during yield. Make sure that the update driver has up to
    // date index information.
    const auto& updateIndexData = CollectionQueryInfo::get(collection()).getIndexKeys(opCtx());
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

bool UpdateStage::checkUpdateChangesShardKeyFields(ScopedCollectionDescription collDesc,
                                                   const Snapshotted<BSONObj>& oldObj) {
    auto newObj = _doc.getObject();
    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    auto oldShardKey = shardKeyPattern.extractShardKeyFromDoc(oldObj.value());
    auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newObj);

    // If the shard key fields remain unchanged by this update we can skip the rest of the checks.
    if (newShardKey.woCompare(oldShardKey) == 0) {
        return false;
    }

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());

    // Assert that the updated doc has no arrays or array descendants for the shard key fields.
    _assertPathsNotArray(_doc, shardKeyPaths);

    // We do not allow modifying shard key value without specifying the full shard key in the query.
    // If the query is a simple equality match on _id, then '_params.canonicalQuery' will be null.
    // But if we are here, we already know that the shard key is not _id, since we have an assertion
    // earlier for requests that try to modify the immutable _id field. So it is safe to uassert if
    // '_params.canonicalQuery' is null OR if the query does not include equality matches on all
    // shard key fields.
    const auto& shardKeyPathsVector = collDesc.getKeyPatternFields();
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
            opCtx()->getTxnNumber() || !opCtx()->writesAreReplicated());

    // At this point we already asserted that the complete shardKey have been specified in the
    // query, this implies that mongos is not doing a broadcast update and that it attached a
    // shardVersion to the command. Thus it is safe to call getOwnershipFilter
    const auto collFilter =
        CollectionShardingState::get(opCtx(), collection()->ns())
            ->getOwnershipFilter(opCtx(),
                                 CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

    // If this document does not belong anymore to this shard
    if (!collFilter.keyBelongsToMe(oldShardKey)) {
        return false;
    }

    if (!collFilter.keyBelongsToMe(newShardKey)) {
        if (MONGO_unlikely(hangBeforeThrowWouldChangeOwningShard.shouldFail())) {
            LOGV2(20605, "Hit hangBeforeThrowWouldChangeOwningShard failpoint");
            hangBeforeThrowWouldChangeOwningShard.pauseWhileSet(opCtx());
        }

        uasserted(WouldChangeOwningShardInfo(oldObj.value(), newObj, false /* upsert */),
                  "This update would cause the doc to change owning shards");
    }

    // We passed all checks, so we will return that this update changes the shard key field, and
    // the updated document will remain on the same node.
    return true;
}

}  // namespace mongo
