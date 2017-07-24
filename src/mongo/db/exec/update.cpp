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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/update.h"

#include <algorithm>

#include "mongo/base/status_with.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

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

Status addObjectIDIdField(mb::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    if (!idElem.ok())
        return {ErrorCodes::BadValue, "Could not create new ObjectId '_id' field.", 17268};

    const auto s = doc->root().pushFront(idElem);
    if (!s.isOK())
        return s;

    return Status::OK();
}

/**
 * Uasserts if any of the paths in 'immutablePaths' are not present in 'document', or if they are
 * arrays or array descendants.
 */
void checkImmutablePathsPresent(const mb::Document& document, const FieldRefSet& immutablePaths) {
    for (auto path = immutablePaths.begin(); path != immutablePaths.end(); ++path) {
        auto elem = document.root();
        for (size_t i = 0; i < (*path)->numParts(); ++i) {
            elem = elem[(*path)->getPart(i)];
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "After applying the update, the new document was missing the "
                                     "required field '"
                                  << (*path)->dottedField()
                                  << "'",
                    elem.ok());
            uassert(
                ErrorCodes::NotSingleValueField,
                str::stream() << "After applying the update to the document, the required field '"
                              << (*path)->dottedField()
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

const std::vector<std::unique_ptr<FieldRef>>* getImmutableFields(OperationContext* opCtx,
                                                                 const NamespaceString& ns) {
    auto metadata = CollectionShardingState::get(opCtx, ns)->getMetadata();
    if (metadata) {
        const std::vector<std::unique_ptr<FieldRef>>& fields = metadata->getKeyPatternFields();
        // Return shard-keys as immutable for the update system.
        return &fields;
    }
    return NULL;
}

}  // namespace

const char* UpdateStage::kStageType = "UPDATE";

UpdateStage::UpdateStage(OperationContext* opCtx,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : PlanStage(kStageType, opCtx),
      _params(params),
      _ws(ws),
      _collection(collection),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new RecordIdSet() : NULL),
      _doc(params.driver->getDocument()) {
    _children.emplace_back(child);
    // We are an update until we fall into the insert case.
    params.driver->setContext(ModifierInterface::ExecInfo::UPDATE_CONTEXT);

    // Before we even start executing, we know whether or not this is a replacement
    // style or $mod style update.
    _specificStats.isDocReplacement = params.driver->isDocReplacement();
}

BSONObj UpdateStage::transformAndUpdate(const Snapshotted<BSONObj>& oldObj, RecordId& recordId) {
    const UpdateRequest* request = _params.request;
    UpdateDriver* driver = _params.driver;
    CanonicalQuery* cq = _params.canonicalQuery;
    UpdateLifecycle* lifecycle = request->getLifecycle();

    // If asked to return new doc, default to the oldObj, in case nothing changes.
    BSONObj newObj = oldObj.value();

    // Ask the driver to apply the mods. It may be that the driver can apply those "in
    // place", that is, some values of the old document just get adjusted without any
    // change to the binary layout on the bson layer. It may be that a whole new document
    // is needed to accomodate the new bson layout of the resulting document. In any event,
    // only enable in-place mutations if the underlying storage engine offers support for
    // writing damage events.
    _doc.reset(oldObj.value(),
               (_collection->updateWithDamagesSupported()
                    ? mutablebson::Document::kInPlaceEnabled
                    : mutablebson::Document::kInPlaceDisabled));

    BSONObj logObj;

    bool docWasModified = false;

    Status status = Status::OK();
    const bool validateForStorage = getOpCtx()->writesAreReplicated() &&
        !request->isFromMigration() && driver->modOptions().enforceOkForStorage;
    FieldRefSet immutablePaths;
    if (getOpCtx()->writesAreReplicated() && !request->isFromMigration()) {
        if (lifecycle) {
            auto immutablePathsVector =
                getImmutableFields(getOpCtx(), request->getNamespaceString());
            if (immutablePathsVector) {
                immutablePaths.fillFrom(
                    transitional_tools_do_not_use::unspool_vector(*immutablePathsVector));
            }
        }
        immutablePaths.keepShortest(&idFieldRef);
    }
    if (!driver->needMatchDetails()) {
        // If we don't need match details, avoid doing the rematch
        status = driver->update(StringData(),
                                oldObj.value(),
                                &_doc,
                                validateForStorage,
                                immutablePaths,
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
                                oldObj.value(),
                                &_doc,
                                validateForStorage,
                                immutablePaths,
                                &logObj,
                                &docWasModified);
    }

    if (!status.isOK()) {
        uasserted(16837, status.reason());
    }

    // Skip adding _id field if the collection is capped (since capped collection documents can
    // neither grow nor shrink).
    const auto createIdField = !_collection->isCapped();

    // Ensure if _id exists it is first
    status = ensureIdFieldIsFirst(&_doc);
    if (status.code() == ErrorCodes::InvalidIdField) {
        // Create ObjectId _id field if we are doing that
        if (createIdField) {
            uassertStatusOK(addObjectIDIdField(&_doc));
        }
    } else {
        uassertStatusOK(status);
    }

    // See if the changes were applied in place
    const char* source = NULL;
    const bool inPlace = _doc.getInPlaceUpdates(&_damages, &source);

    if (inPlace && _damages.empty()) {
        // An interesting edge case. A modifier didn't notice that it was really a no-op
        // during its 'prepare' phase. That represents a missed optimization, but we still
        // shouldn't do any real work. Toggle 'docWasModified' to 'false'.
        //
        // Currently, an example of this is '{ $pushAll : { x : [] } }' when the 'x' array
        // exists.
        docWasModified = false;
    }

    if (docWasModified) {

        // Prepare to write back the modified document
        WriteUnitOfWork wunit(getOpCtx());

        RecordId newRecordId;

        if (inPlace) {
            // Don't actually do the write if this is an explain.
            if (!request->isExplain()) {
                invariant(_collection);
                newObj = oldObj.value();
                const RecordData oldRec(oldObj.value().objdata(), oldObj.value().objsize());
                BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request->isMulti());
                OplogUpdateEntryArgs args;
                args.nss = _collection->ns();
                args.stmtId = request->getStmtId();
                args.update = logObj;
                args.criteria = idQuery;
                args.fromMigrate = request->isFromMigration();
                StatusWith<RecordData> newRecStatus = _collection->updateDocumentWithDamages(
                    getOpCtx(),
                    recordId,
                    Snapshotted<RecordData>(oldObj.snapshotId(), oldRec),
                    source,
                    _damages,
                    &args);
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

            // Don't actually do the write if this is an explain.
            if (!request->isExplain()) {
                invariant(_collection);
                BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request->isMulti());
                OplogUpdateEntryArgs args;
                args.nss = _collection->ns();
                args.uuid = _collection->uuid();
                args.stmtId = request->getStmtId();
                args.update = logObj;
                args.criteria = idQuery;
                args.fromMigrate = request->isFromMigration();
                StatusWith<RecordId> res = _collection->updateDocument(getOpCtx(),
                                                                       recordId,
                                                                       oldObj,
                                                                       newObj,
                                                                       true,
                                                                       driver->modsAffectIndices(),
                                                                       _params.opDebug,
                                                                       &args);
                uassertStatusOK(res.getStatus());
                newRecordId = res.getValue();
            }
        }

        invariant(oldObj.snapshotId() == getOpCtx()->recoveryUnit()->getSnapshotId());
        wunit.commit();

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

Status UpdateStage::applyUpdateOpsForInsert(OperationContext* opCtx,
                                            const CanonicalQuery* cq,
                                            const BSONObj& query,
                                            UpdateDriver* driver,
                                            mutablebson::Document* doc,
                                            bool isInternalRequest,
                                            const NamespaceString& ns,
                                            UpdateStats* stats,
                                            BSONObj* out) {
    // Since this is an insert (no docs found and upsert:true), we will be logging it
    // as an insert in the oplog. We don't need the driver's help to build the
    // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
    // Some mods may only work in that context (e.g. $setOnInsert).
    driver->setLogOp(false);
    driver->setContext(ModifierInterface::ExecInfo::INSERT_CONTEXT);

    FieldRefSet immutablePaths;
    if (!isInternalRequest) {
        auto immutablePathsVector = getImmutableFields(opCtx, ns);
        if (immutablePathsVector) {
            immutablePaths.fillFrom(
                transitional_tools_do_not_use::unspool_vector(*immutablePathsVector));
        }
    }
    immutablePaths.keepShortest(&idFieldRef);

    // The original document we compare changes to - immutable paths must not change
    BSONObj original;

    if (cq) {
        Status status = driver->populateDocumentWithQueryFields(*cq, immutablePaths, *doc);
        if (!status.isOK()) {
            return status;
        }

        if (driver->isDocReplacement())
            stats->fastmodinsert = true;
        original = doc->getObject();
    } else {
        fassert(17354, CanonicalQuery::isSimpleIdQuery(query));
        BSONElement idElt = query[idFieldName];
        original = idElt.wrap();
        fassert(17352, doc->root().appendElement(idElt));
    }

    // Apply the update modifications here. Do not validate for storage, since we will validate the
    // entire document after the update. However, we ensure that no immutable fields are updated.
    const bool validateForStorage = false;
    if (isInternalRequest) {
        immutablePaths.clear();
    }
    Status updateStatus =
        driver->update(StringData(), original, doc, validateForStorage, immutablePaths);
    if (!updateStatus.isOK()) {
        return Status(updateStatus.code(), updateStatus.reason(), 16836);
    }

    // Ensure _id exists and is first
    auto idAndFirstStatus = ensureIdFieldIsFirst(doc);
    if (idAndFirstStatus.code() == ErrorCodes::InvalidIdField) {  // _id field is missing
        idAndFirstStatus = addObjectIDIdField(doc);
    }

    if (!idAndFirstStatus.isOK()) {
        return idAndFirstStatus;
    }

    // Validate that the object replacement or modifiers resulted in a document
    // that contains all the immutable keys and can be stored if it isn't coming
    // from a migration or via replication.
    if (!isInternalRequest) {
        if (driver->modOptions().enforceOkForStorage) {
            storage_validation::storageValid(*doc);
        }
        checkImmutablePathsPresent(*doc, immutablePaths);
    }

    BSONObj newObj = doc->getObject();
    if (newObj.objsize() > BSONObjMaxUserSize) {
        return Status(ErrorCodes::InvalidBSON,
                      str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
                      17420);
    }

    *out = newObj;
    return Status::OK();
}

void UpdateStage::doInsert() {
    _specificStats.inserted = true;

    const UpdateRequest* request = _params.request;
    bool isInternalRequest = !getOpCtx()->writesAreReplicated() || request->isFromMigration();

    // Reset the document we will be writing to.
    _doc.reset();

    BSONObj newObj;
    uassertStatusOK(applyUpdateOpsForInsert(getOpCtx(),
                                            _params.canonicalQuery,
                                            request->getQuery(),
                                            _params.driver,
                                            &_doc,
                                            isInternalRequest,
                                            request->getNamespaceString(),
                                            &_specificStats,
                                            &newObj));

    _specificStats.objInserted = newObj;

    // If this is an explain, bail out now without doing the insert.
    if (request->isExplain()) {
        return;
    }

    writeConflictRetry(getOpCtx(), "upsert", _collection->ns().ns(), [&] {
        WriteUnitOfWork wunit(getOpCtx());
        invariant(_collection);
        const bool enforceQuota = !request->isGod();
        uassertStatusOK(_collection->insertDocument(getOpCtx(),
                                                    InsertStatement(request->getStmtId(), newObj),
                                                    _params.opDebug,
                                                    enforceQuota,
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
            // TODO we may want to handle WriteConflictException here. Currently we bounce it
            // out to a higher level since if this WCEs it is likely that we raced with another
            // upsert that may have matched our query, and therefore this may need to perform an
            // update rather than an insert. Bouncing to the higher level allows restarting the
            // query in this case.
            doInsert();

            invariant(isEOF());
            if (_params.request->shouldReturnNewDocs()) {
                // Want to return the document we just inserted, create it as a WorkingSetMember
                // so that we can return it.
                BSONObj newObj = _specificStats.objInserted;
                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->obj = Snapshotted<BSONObj>(getOpCtx()->recoveryUnit()->getSnapshotId(),
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

    // If we're here, then we still have to ask for results from the child and apply
    // updates to them. We should only get here if the collection exists.
    invariant(_collection);

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
        ScopeGuard memberFreer = MakeGuard(&WorkingSet::free, _ws, id);

        if (!member->hasRecordId()) {
            // We expect to be here because of an invalidation causing a force-fetch.
            ++_specificStats.nInvalidateSkips;
            return PlanStage::NEED_TIME;
        }
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
                _collection, getOpCtx(), _ws, id, _params.canonicalQuery);
        } catch (const WriteConflictException&) {
            // There was a problem trying to detect if the document still exists, so retry.
            memberFreer.Dismiss();
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

        // Save state before making changes
        WorkingSetCommon::prepareForSnapshotChange(_ws);
        try {
            child()->saveState();
        } catch (const WriteConflictException&) {
            std::terminate();
        }

        // If we care about the pre-updated version of the doc, save it out here.
        BSONObj oldObj;
        if (_params.request->shouldReturnOldDocs()) {
            oldObj = member->obj.value().getOwned();
        }

        BSONObj newObj;
        try {
            // Do the update, get us the new version of the doc.
            newObj = transformAndUpdate(member->obj, recordId);
        } catch (const WriteConflictException&) {
            memberFreer.Dismiss();  // Keep this member around so we can retry updating it.
            return prepareToRetryWSM(id, out);
        }

        // Set member's obj to be the doc we want to return.
        if (_params.request->shouldReturnAnyDocs()) {
            if (_params.request->shouldReturnNewDocs()) {
                member->obj = Snapshotted<BSONObj>(getOpCtx()->recoveryUnit()->getSnapshotId(),
                                                   newObj.getOwned());
            } else {
                invariant(_params.request->shouldReturnOldDocs());
                member->obj.setValue(oldObj);
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
                memberFreer.Dismiss();
            }
            *out = WorkingSet::INVALID_ID;
            return NEED_YIELD;
        }

        if (_params.request->shouldReturnAnyDocs()) {
            // member->obj should refer to the document we want to return.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            memberFreer.Dismiss();  // Keep this member around so we can return it.
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

Status UpdateStage::restoreUpdateState() {
    const UpdateRequest& request = *_params.request;
    const NamespaceString& nsString(request.getNamespaceString());

    // We may have stepped down during the yield.
    bool userInitiatedWritesAndNotPrimary = getOpCtx()->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(getOpCtx(), nsString);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::PrimarySteppedDown,
                      str::stream() << "Demoted from primary while performing update on "
                                    << nsString.ns());
    }

    if (request.getLifecycle()) {
        UpdateLifecycle* lifecycle = request.getLifecycle();
        lifecycle->setCollection(_collection);

        if (!lifecycle->canContinue()) {
            return Status(ErrorCodes::IllegalOperation,
                          "Update aborted due to invalid state transitions after yield.",
                          17270);
        }

        _params.driver->refreshIndexKeys(lifecycle->getIndexKeys(getOpCtx()));
    }

    return Status::OK();
}

void UpdateStage::doRestoreState() {
    uassertStatusOK(restoreUpdateState());
}

unique_ptr<PlanStageStats> UpdateStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_UPDATE);
    ret->specific = make_unique<UpdateStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* UpdateStage::getSpecificStats() const {
    return &_specificStats;
}

const UpdateStats* UpdateStage::getUpdateStats(const PlanExecutor* exec) {
    invariant(exec->getRootStage()->isEOF());
    invariant(exec->getRootStage()->stageType() == STAGE_UPDATE);
    UpdateStage* updateStage = static_cast<UpdateStage*>(exec->getRootStage());
    return static_cast<const UpdateStats*>(updateStage->getSpecificStats());
}

void UpdateStage::recordUpdateStatsInOpDebug(const UpdateStats* updateStats, OpDebug* opDebug) {
    invariant(opDebug);
    opDebug->nMatched = updateStats->nMatched;
    opDebug->nModified = updateStats->nModified;
    opDebug->upsert = updateStats->inserted;
    opDebug->fastmodinsert = updateStats->fastmodinsert;
}

UpdateResult UpdateStage::makeUpdateResult(const UpdateStats* updateStats) {
    return UpdateResult(updateStats->nMatched > 0 /* Did we update at least one obj? */,
                        !updateStats->isDocReplacement /* $mod or obj replacement */,
                        updateStats->nModified /* number of modified docs, no no-ops */,
                        updateStats->nMatched /* # of docs matched/updated, even no-ops */,
                        updateStats->objInserted);
};

PlanStage::StageState UpdateStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

}  // namespace mongo
