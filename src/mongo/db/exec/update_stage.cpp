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


#include "mongo/platform/basic.h"

#include "mongo/db/exec/update_stage.h"

#include <algorithm>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


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

// Public constructor.
UpdateStage::UpdateStage(ExpressionContext* expCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         const CollectionPtr& collection,
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
                         const CollectionPtr& collection)
    : RequiresMutableCollectionStage(kStageType.rawData(), expCtx, collection),
      _params(params),
      _ws(ws),
      _doc(params.driver->getDocument()),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new RecordIdSet() : nullptr),
      _preWriteFilter(opCtx(), collection->ns()) {

    // Should the modifiers validate their embedded docs via storage_validation::scanDocument()?
    // Only user updates should be checked. Any system or replication stuff should pass through.
    const auto request = _params.request;

    _isUserInitiatedWrite = opCtx()->writesAreReplicated() &&
        !(request->isFromOplogApplication() ||
          params.driver->type() == UpdateDriver::UpdateType::kDelta ||
          request->source() == OperationSource::kFromMigrate);

    _specificStats.isModUpdate = params.driver->type() == UpdateDriver::UpdateType::kOperator;
}

BSONObj UpdateStage::transformAndUpdate(const Snapshotted<BSONObj>& oldObj,
                                        RecordId& recordId,
                                        bool writeToOrphan) {
    const UpdateRequest* const request = _params.request;
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

    Status status = Status::OK();
    const bool isInsert = false;
    FieldRefSet immutablePaths;

    if (_isUserInitiatedWrite) {
        // Documents coming directly from users should be validated for storage. It is safe to
        // access the CollectionShardingState in this write context and to throw SSV if the sharding
        // metadata has not been initialized.
        const auto collDesc = CollectionShardingState::get(opCtx(), collection()->ns())
                                  ->getCollectionDescription(opCtx());
        if (collDesc.isSharded() && !OperationShardingState::isComingFromRouter(opCtx())) {
            immutablePaths.fillFrom(collDesc.getKeyPatternFields());
        }

        immutablePaths.keepShortest(&idFieldRef);
    }

    if (!driver->needMatchDetails()) {
        // If we don't need match details, avoid doing the rematch
        status = driver->update(opCtx(),
                                StringData(),
                                &_doc,
                                _isUserInitiatedWrite,
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

        status = driver->update(opCtx(),
                                matchedField,
                                &_doc,
                                _isUserInitiatedWrite,
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

        if (!request->explain()) {
            args.stmtIds = request->getStmtIds();
            args.update = logObj;
            if (_isUserInitiatedWrite) {
                args.criteria = CollectionShardingState::get(opCtx(), collection()->ns())
                                    ->getCollectionDescription(opCtx())
                                    .extractDocumentKey(newObj);
            } else {
                const auto docId = newObj[idFieldName];
                args.criteria = docId ? docId.wrap() : newObj;
            }
            uassert(16980,
                    "Multi-update operations require all documents to have an '_id' field",
                    !request->isMulti() || args.criteria.hasField("_id"_sd));
            args.storeDocOption = getStoreDocMode(*request);
            if (args.storeDocOption == CollectionUpdateArgs::StoreDocOption::PreImage) {
                args.preImageDoc = oldObj.value().getOwned();
            }
        }

        // Ensure we set the type correctly
        args.source = writeToOrphan ? OperationSource::kFromMigrate : request->source();

        if (inPlace) {
            if (!request->explain()) {
                newObj = oldObj.value();
                const RecordData oldRec(oldObj.value().objdata(), oldObj.value().objsize());

                Snapshotted<RecordData> snap(oldObj.snapshotId(), oldRec);

                if (_isUserInitiatedWrite &&
                    checkUpdateChangesShardKeyFields(boost::none, oldObj) && !args.preImageDoc) {
                    args.preImageDoc = oldObj.value().getOwned();
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
            if (!DocumentValidationSettings::get(opCtx()).isInternalValidationDisabled()) {
                uassert(17419,
                        str::stream() << "Resulting document after update is larger than "
                                      << BSONObjMaxUserSize,
                        newObj.objsize() <= BSONObjMaxUserSize);
            }

            if (!request->explain()) {
                if (_isUserInitiatedWrite && checkUpdateChangesShardKeyFields(newObj, oldObj) &&
                    !args.preImageDoc) {
                    args.preImageDoc = oldObj.value().getOwned();
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
    if (docWasModified || request->explain()) {
        _specificStats.nModified += _params.numStatsForDoc ? _params.numStatsForDoc(newObj) : 1;
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

    boost::optional<repl::UnreplicatedWritesBlock> unReplBlock;
    if (collection()->ns().isImplicitlyReplicated() && !_isUserInitiatedWrite) {
        // Implictly replicated collections do not replicate updates.
        // However, user-initiated writes and some background maintenance tasks are allowed
        // to replicate as they cannot be derived from the oplog.
        unReplBlock.emplace(opCtx());
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
        ScopeGuard memberFreer([&] { _ws->free(id); });

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
        const auto ensureStillMatchesRet =
            handlePlanStageYield(expCtx(),
                                 "UpdateStage ensureStillMatches",
                                 collection()->ns().ns(),
                                 [&] {
                                     docStillMatches = write_stage_common::ensureStillMatches(
                                         collection(), opCtx(), _ws, id, _params.canonicalQuery);
                                     return PlanStage::NEED_TIME;
                                 },
                                 [&] {
                                     // yieldHandler
                                     // There was a problem trying to detect if the document still
                                     // exists, so retry.
                                     memberFreer.dismiss();
                                     prepareToRetryWSM(id, out);
                                 });

        if (ensureStillMatchesRet != PlanStage::NEED_TIME) {
            return ensureStillMatchesRet;
        }

        if (!docStillMatches) {
            // Either the document has been deleted, or it has been updated such that it no longer
            // matches the predicate.
            if (shouldRestartUpdateIfNoLongerMatches(_params)) {
                throwWriteConflictException("Document no longer matches the predicate.");
            }
            return PlanStage::NEED_TIME;
        }

        bool writeToOrphan = false;
        if (!_params.request->explain() && _isUserInitiatedWrite) {
            try {
                const auto action = _preWriteFilter.computeAction(member->doc.value());
                if (action == write_stage_common::PreWriteFilter::Action::kSkip) {
                    LOGV2_DEBUG(
                        5983200,
                        3,
                        "Skipping update operation to orphan document to prevent a wrong change "
                        "stream event",
                        "namespace"_attr = collection()->ns(),
                        "record"_attr = member->doc.value());
                    return PlanStage::NEED_TIME;
                } else if (action ==
                           write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                    LOGV2_DEBUG(
                        6184701,
                        3,
                        "Marking update operation to orphan document with the fromMigrate flag "
                        "to prevent a wrong change stream event",
                        "namespace"_attr = collection()->ns(),
                        "record"_attr = member->doc.value());
                    writeToOrphan = true;
                }
            } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                if (ex->getVersionReceived() == ShardVersion::IGNORED() &&
                    ex->getCriticalSectionSignal()) {
                    // If ShardVersion is IGNORED and we encountered a critical section, then yield,
                    // wait for critical section to finish and then we'll resume the write from the
                    // point we had left. We do this to prevent large multi-writes from repeatedly
                    // failing due to StaleConfig and exhausting the mongos retry attempts.
                    planExecutorShardingCriticalSectionFuture(opCtx()) =
                        ex->getCriticalSectionSignal();
                    memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
                    prepareToRetryWSM(id, out);
                    return PlanStage::NEED_YIELD;
                }
                throw;
            }
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState()
        // is allowed to free the memory.
        member->makeObjOwnedIfNeeded();

        // Save state before making changes.
        handlePlanStageYield(expCtx(),
                             "UpdateStage saveState",
                             collection()->ns().ns(),
                             [&] {
                                 child()->saveState();
                                 return PlanStage::NEED_TIME /* unused */;
                             },
                             [&] {
                                 // yieldHandler
                                 std::terminate();
                             });

        // If we care about the pre-updated version of the doc, save it out here.
        BSONObj oldObj;
        SnapshotId oldSnapshot = member->doc.snapshotId();
        if (_params.request->shouldReturnOldDocs()) {
            oldObj = member->doc.value().toBson().getOwned();
        }

        BSONObj newObj;

        try {
            const auto updateRet = handlePlanStageYield(
                expCtx(),
                "UpdateStage update",
                collection()->ns().ns(),
                [&] {
                    // Do the update, get us the new version of the doc.
                    newObj = transformAndUpdate(
                        {oldSnapshot, member->doc.value().toBson()}, recordId, writeToOrphan);
                    return PlanStage::NEED_TIME;
                },
                [&] {
                    // yieldHandler
                    memberFreer.dismiss();  // Keep this member around so we can retry updating it.
                    prepareToRetryWSM(id, out);
                });

            if (updateRet != PlanStage::NEED_TIME) {
                return updateRet;
            }
        } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
            if (ex->getVersionReceived() == ShardVersion::IGNORED() &&
                ex->getCriticalSectionSignal()) {
                // If ShardVersion is IGNORED and we encountered a critical section, then yield,
                // wait for critical section to finish and then we'll resume the write from the
                // point we had left. We do this to prevent large multi-writes from repeatedly
                // failing due to StaleConfig and exhausting the mongos retry attempts.
                planExecutorShardingCriticalSectionFuture(opCtx()) = ex->getCriticalSectionSignal();
                memberFreer.dismiss();  // Keep this member around so we can retry updating it.
                prepareToRetryWSM(id, out);
                return PlanStage::NEED_YIELD;
            }
            throw;
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
        _specificStats.nMatched += _params.numStatsForDoc ? _params.numStatsForDoc(newObj) : 1;

        // Restore state after modification. As restoreState may restore (recreate) cursors, make
        // sure to restore the state outside of the WritUnitOfWork.
        const auto restoreStateRet = handlePlanStageYield(
            expCtx(),
            "UpdateStage restoreState",
            collection()->ns().ns(),
            [&] {
                child()->restoreState(&collection());
                return PlanStage::NEED_TIME;
            },
            [&] {
                // yieldHandler
                // Note we don't need to retry updating anything in this case since the update
                // already was committed. However, we still need to return the updated document (if
                // it was requested).
                if (_params.request->shouldReturnAnyDocs()) {
                    // member->obj should refer to the document we want to return.
                    invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

                    _idReturning = id;
                    // Keep this member around so that we can return it on the next
                    // work() call.
                    memberFreer.dismiss();
                }
                *out = WorkingSet::INVALID_ID;
            });

        if (restoreStateRet != PlanStage::NEED_TIME) {
            return restoreStateRet;
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

    _preWriteFilter.restoreState();
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

void UpdateStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
}


void UpdateStage::_checkRestrictionsOnUpdatingShardKeyAreNotViolated(
    const ScopedCollectionDescription& collDesc, const FieldRefSet& shardKeyPaths) {
    // We do not allow modifying either the current shard key value or new shard key value (if
    // resharding) without specifying the full current shard key in the query.
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
    // An update without a transaction number is legal if
    // gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi is enabled because mongos
    // will be able to start an internal transaction to handle the wouldChangeOwningShard error
    // thrown below.
    // If this node is a replica set secondary node, we can skip validation.
    if (!feature_flags::gFeatureFlagUpdateDocumentShardKeyUsingTransactionApi.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        uassert(ErrorCodes::IllegalOperation,
                "Must run update to shard key field in a multi-statement transaction or with "
                "retryWrites: true.",
                opCtx()->getTxnNumber());
    }
}


bool UpdateStage::wasReshardingKeyUpdated(const ShardingWriteRouter& shardingWriteRouter,
                                          const ScopedCollectionDescription& collDesc,
                                          const BSONObj& newObj,
                                          const Snapshotted<BSONObj>& oldObj) {
    auto reshardingKeyPattern = collDesc.getReshardingKeyIfShouldForwardOps();
    if (!reshardingKeyPattern)
        return false;

    auto oldShardKey = reshardingKeyPattern->extractShardKeyFromDoc(oldObj.value());
    auto newShardKey = reshardingKeyPattern->extractShardKeyFromDoc(newObj);

    if (newShardKey.binaryEqual(oldShardKey))
        return false;

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());
    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(collDesc, shardKeyPaths);

    auto oldRecipShard = *shardingWriteRouter.getReshardingDestinedRecipient(oldObj.value());
    auto newRecipShard = *shardingWriteRouter.getReshardingDestinedRecipient(newObj);

    uassert(
        WouldChangeOwningShardInfo(
            oldObj.value(), newObj, false /* upsert */, collection()->ns(), collection()->uuid()),
        "This update would cause the doc to change owning shards under the new shard key",
        oldRecipShard == newRecipShard);

    return true;
}

bool UpdateStage::checkUpdateChangesShardKeyFields(const boost::optional<BSONObj>& newObjCopy,
                                                   const Snapshotted<BSONObj>& oldObj) {
    ShardingWriteRouter shardingWriteRouter(
        opCtx(), collection()->ns(), Grid::get(opCtx())->catalogCache());
    auto css = shardingWriteRouter.getCollectionShardingState();

    // css can be null when this is a config server.
    if (css == nullptr) {
        return false;
    }

    const auto collDesc = css->getCollectionDescription(opCtx());

    // Calling mutablebson::Document::getObject() renders a full copy of the updated document. This
    // can be expensive for larger documents, so we skip calling it when the collection isn't even
    // sharded.
    if (!collDesc.isSharded()) {
        return false;
    }

    const auto& newObj = newObjCopy ? *newObjCopy : _doc.getObject();

    // It is possible that both the existing and new shard keys are being updated, so we do not want
    // to short-circuit checking whether either is being modified.
    const auto existingShardKeyUpdated =
        wasExistingShardKeyUpdated(shardingWriteRouter, collDesc, newObj, oldObj);
    const auto reshardingKeyUpdated =
        wasReshardingKeyUpdated(shardingWriteRouter, collDesc, newObj, oldObj);

    return existingShardKeyUpdated || reshardingKeyUpdated;
}

bool UpdateStage::wasExistingShardKeyUpdated(const ShardingWriteRouter& shardingWriteRouter,
                                             const ScopedCollectionDescription& collDesc,
                                             const BSONObj& newObj,
                                             const Snapshotted<BSONObj>& oldObj) {
    const auto css = shardingWriteRouter.getCollectionShardingState();

    const ShardKeyPattern& shardKeyPattern = collDesc.getShardKeyPattern();
    auto oldShardKey = shardKeyPattern.extractShardKeyFromDoc(oldObj.value());
    auto newShardKey = shardKeyPattern.extractShardKeyFromDoc(newObj);

    // If the shard key fields remain unchanged by this update we can skip the rest of the checks.
    // Using BSONObj::binaryEqual() still allows a missing shard key field to be filled in with an
    // explicit null value.
    if (newShardKey.binaryEqual(oldShardKey)) {
        return false;
    }

    FieldRefSet shardKeyPaths(collDesc.getKeyPatternFields());

    // Assert that the updated doc has no arrays or array descendants for the shard key fields.
    _assertPathsNotArray(_doc, shardKeyPaths);

    _checkRestrictionsOnUpdatingShardKeyAreNotViolated(collDesc, shardKeyPaths);

    // At this point we already asserted that the complete shardKey have been specified in the
    // query, this implies that mongos is not doing a broadcast update and that it attached a
    // shardVersion to the command. Thus it is safe to call getOwnershipFilter
    const auto collFilter = css->getOwnershipFilter(
        opCtx(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

    // If the shard key of an orphan document is allowed to change, and the document is allowed to
    // become owned by the shard, the global uniqueness assumption for _id values would be violated.
    invariant(collFilter.keyBelongsToMe(oldShardKey));

    if (!collFilter.keyBelongsToMe(newShardKey)) {
        if (MONGO_unlikely(hangBeforeThrowWouldChangeOwningShard.shouldFail())) {
            LOGV2(20605, "Hit hangBeforeThrowWouldChangeOwningShard failpoint");
            hangBeforeThrowWouldChangeOwningShard.pauseWhileSet(opCtx());
        }

        uasserted(WouldChangeOwningShardInfo(oldObj.value(),
                                             newObj,
                                             false /* upsert */,
                                             collection()->ns(),
                                             collection()->uuid()),
                  "This update would cause the doc to change owning shards");
    }

    // We passed all checks, so we will return that this update changes the shard key field, and
    // the updated document will remain on the same node.
    return true;
}

}  // namespace mongo
