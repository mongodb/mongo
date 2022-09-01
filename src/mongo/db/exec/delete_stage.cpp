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

#include "mongo/db/exec/delete_stage.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

using std::unique_ptr;
using std::vector;

namespace {

/**
 * Returns true if we should throw a WriteConflictException in order to retry the operation in
 * the case of a conflict. Returns false if we should skip the document and keep going.
 */
bool shouldRestartDeleteIfNoLongerMatches(const DeleteStageParams* params) {
    // When we're doing a findAndModify with a sort, the sort will have a limit of 1, so it will not
    // produce any more results even if there is another matching document. Throw a WCE here so that
    // these operations get another chance to find a matching document. The findAndModify command
    // should automatically retry if it gets a WCE.
    return params->returnDeleted && !params->sort.isEmpty();
};

}  // namespace

DeleteStage::DeleteStage(ExpressionContext* expCtx,
                         std::unique_ptr<DeleteStageParams> params,
                         WorkingSet* ws,
                         const CollectionPtr& collection,
                         PlanStage* child)
    : DeleteStage(kStageType.rawData(), expCtx, std::move(params), ws, collection, child) {}

DeleteStage::DeleteStage(const char* stageType,
                         ExpressionContext* expCtx,
                         std::unique_ptr<DeleteStageParams> params,
                         WorkingSet* ws,
                         const CollectionPtr& collection,
                         PlanStage* child)
    : RequiresMutableCollectionStage(stageType, expCtx, collection),
      _params(std::move(params)),
      _ws(ws),
      _preWriteFilter(opCtx(), collection->ns()),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID) {
    _children.emplace_back(child);
}

bool DeleteStage::isEOF() {
    if (!_params->isMulti && _specificStats.docsDeleted > 0) {
        return true;
    }
    return _idRetrying == WorkingSet::INVALID_ID && _idReturning == WorkingSet::INVALID_ID &&
        child()->isEOF();
}

PlanStage::StageState DeleteStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // It is possible that after a delete was executed, a WriteConflictException occurred
    // and prevented us from returning ADVANCED with the old version of the document.
    if (_idReturning != WorkingSet::INVALID_ID) {
        // We should only get here if we were trying to return something before.
        invariant(_params->returnDeleted);

        WorkingSetMember* member = _ws->get(_idReturning);
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        *out = _idReturning;
        _idReturning = WorkingSet::INVALID_ID;
        return PlanStage::ADVANCED;
    }

    // Either retry the last WSM we worked on or get a new one from our child.
    WorkingSetID id;
    if (_idRetrying != WorkingSet::INVALID_ID) {
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    } else {
        auto status = child()->work(&id);

        switch (status) {
            case PlanStage::ADVANCED:
                break;

            case PlanStage::NEED_TIME:
                return status;

            case PlanStage::NEED_YIELD:
                *out = id;
                return status;

            case PlanStage::IS_EOF:
                return status;

            default:
                MONGO_UNREACHABLE;
        }
    }

    // We advanced, or are retrying, and id is set to the WSM to work on.
    WorkingSetMember* member = _ws->get(id);

    // We want to free this member when we return, unless we need to retry deleting or returning it.
    ScopeGuard memberFreer([&] { _ws->free(id); });

    invariant(member->hasRecordId());
    // It's safe to have a reference instead of a copy here due to the member pointer only being
    // invalidated if the memberFreer ScopeGuard activates. This will only be the case if the
    // document is deleted successfully and thus the existing RecordId becomes invalid.
    const auto& recordId = member->recordId;
    // Deletes can't have projections. This means that covering analysis will always add
    // a fetch. We should always get fetched data, and never just key data.
    invariant(member->hasObj());

    // Ensure the document still exists and matches the predicate.
    bool docStillMatches;

    const auto ret =
        handlePlanStageYield(expCtx(),
                             "DeleteStage ensureStillMatches",
                             collection()->ns().ns(),
                             [&] {
                                 docStillMatches = write_stage_common::ensureStillMatches(
                                     collection(), opCtx(), _ws, id, _params->canonicalQuery);
                                 return PlanStage::NEED_TIME;
                             },
                             [&] {
                                 // yieldHandler
                                 // There was a problem trying to detect if the document still
                                 // exists, so retry.
                                 memberFreer.dismiss();
                                 prepareToRetryWSM(id, out);
                             });

    if (ret != PlanStage::NEED_TIME) {
        return ret;
    }

    if (!docStillMatches) {
        // Either the document has already been deleted, or it has been updated such that it no
        // longer matches the predicate.
        if (shouldRestartDeleteIfNoLongerMatches(_params.get())) {
            throwWriteConflictException("Document no longer matches the predicate.");
        }
        return PlanStage::NEED_TIME;
    }

    bool writeToOrphan = false;
    if (!_params->isExplain && !_params->fromMigrate) {
        try {
            const auto action = _preWriteFilter.computeAction(member->doc.value());
            if (action == write_stage_common::PreWriteFilter::Action::kSkip) {
                LOGV2_DEBUG(
                    5983201,
                    3,
                    "Skipping delete operation to orphan document to prevent a wrong change "
                    "stream event",
                    "namespace"_attr = collection()->ns(),
                    "record"_attr = member->doc.value());
                return PlanStage::NEED_TIME;
            } else if (action == write_stage_common::PreWriteFilter::Action::kWriteAsFromMigrate) {
                LOGV2_DEBUG(6184700,
                            3,
                            "Marking delete operation to orphan document with the fromMigrate flag "
                            "to prevent a wrong change stream event",
                            "namespace"_attr = collection()->ns(),
                            "record"_attr = member->doc.value());
                writeToOrphan = true;
            }
        } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
            if (ex->getVersionReceived() == ShardVersion::IGNORED() &&
                ex->getCriticalSectionSignal()) {
                // If ShardVersion is IGNORED and we encountered a critical section, then yield,
                // wait for the critical section to finish and then we'll resume the write from the
                // point we had left. We do this to prevent large multi-writes from repeatedly
                // failing due to StaleConfig and exhausting the mongos retry attempts.
                planExecutorShardingCriticalSectionFuture(opCtx()) = ex->getCriticalSectionSignal();
                memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
                prepareToRetryWSM(id, out);
                return PlanStage::NEED_YIELD;
            }
            throw;
        }
    }

    // Ensure that the BSONObj underlying the WSM is owned because saveState() is
    // allowed to free the memory the BSONObj points to. The BSONObj will be needed
    // later when it is passed to Collection::deleteDocument(). Note that the call to
    // makeObjOwnedIfNeeded() will leave the WSM in the RID_AND_OBJ state in case we need to retry
    // deleting it.
    member->makeObjOwnedIfNeeded();

    Snapshotted<Document> memberDoc = member->doc;
    BSONObj bsonObjDoc = memberDoc.value().toBson();

    if (_params->removeSaver) {
        uassertStatusOK(_params->removeSaver->goingToDelete(bsonObjDoc));
    }

    handlePlanStageYield(expCtx(),
                         "DeleteStage saveState",
                         collection()->ns().ns(),
                         [&] {
                             child()->saveState();
                             return PlanStage::NEED_TIME /* unused */;
                         },
                         [&] {
                             // yieldHandler
                             std::terminate();
                         });

    // Do the write, unless this is an explain.
    if (!_params->isExplain) {
        try {
            const auto ret = handlePlanStageYield(
                expCtx(),
                "DeleteStage deleteDocument",
                collection()->ns().ns(),
                [&] {
                    WriteUnitOfWork wunit(opCtx());
                    collection()->deleteDocument(opCtx(),
                                                 Snapshotted(memberDoc.snapshotId(), bsonObjDoc),
                                                 _params->stmtId,
                                                 recordId,
                                                 _params->opDebug,
                                                 writeToOrphan || _params->fromMigrate,
                                                 false,
                                                 _params->returnDeleted
                                                     ? Collection::StoreDeletedDoc::On
                                                     : Collection::StoreDeletedDoc::Off);
                    wunit.commit();
                    return PlanStage::NEED_TIME;
                },
                [&] {
                    // yieldHandler
                    memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
                    prepareToRetryWSM(id, out);
                });
            if (ret != PlanStage::NEED_TIME) {
                return ret;
            }
        } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
            if (ex->getVersionReceived() == ShardVersion::IGNORED() &&
                ex->getCriticalSectionSignal()) {
                // If ShardVersion is IGNORED and we encountered a critical section, then yield,
                // wait for the critical section to finish and then we'll resume the write from the
                // point we had left. We do this to prevent large multi-writes from repeatedly
                // failing due to StaleConfig and exhausting the mongos retry attempts.
                planExecutorShardingCriticalSectionFuture(opCtx()) = ex->getCriticalSectionSignal();
                memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
                prepareToRetryWSM(id, out);
                return PlanStage::NEED_YIELD;
            }
            throw;
        }
    }
    _specificStats.docsDeleted += _params->numStatsForDoc ? _params->numStatsForDoc(bsonObjDoc) : 1;
    _specificStats.bytesDeleted += bsonObjDoc.objsize();

    if (_params->returnDeleted) {
        // After deleting the document, the RecordId associated with this member is invalid.
        // Remove the 'recordId' from the WorkingSetMember before returning it.
        member->recordId = RecordId();
        member->transitionToOwnedObj();
    }

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in
    // which they are created, and a WriteUnitOfWork is a transaction, make sure to restore the
    // state outside of the WriteUnitOfWork.
    const auto restoreStateRet =
        handlePlanStageYield(expCtx(),
                             "DeleteStage restoreState",
                             collection()->ns().ns(),
                             [&] {
                                 child()->restoreState(&collection());
                                 return PlanStage::NEED_TIME;
                             },
                             [&] {
                                 // yieldHandler
                                 // Note we don't need to retry anything in this case since the
                                 // delete already was committed. However, we still need to return
                                 // the deleted document (if it was requested).
                                 if (_params->returnDeleted) {
                                     // member->obj should refer to the deleted document.
                                     invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

                                     _idReturning = id;
                                     // Keep this member around so that we can return it on
                                     // the next work() call.
                                     memberFreer.dismiss();
                                 }
                                 *out = WorkingSet::INVALID_ID;
                             });
    if (restoreStateRet != PlanStage::NEED_TIME) {
        return ret;
    }

    if (_params->returnDeleted) {
        // member->obj should refer to the deleted document.
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        memberFreer.dismiss();  // Keep this member around so we can return it.
        *out = id;
        return PlanStage::ADVANCED;
    }

    return PlanStage::NEED_TIME;
}

void DeleteStage::doRestoreStateRequiresCollection() {
    const NamespaceString& ns = collection()->ns();
    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Demoted from primary while removing from " << ns.ns(),
            !opCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), ns));

    _preWriteFilter.restoreState();
}

unique_ptr<PlanStageStats> DeleteStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_DELETE);
    ret->specific = std::make_unique<DeleteStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* DeleteStage::getSpecificStats() const {
    return &_specificStats;
}

void DeleteStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
}

}  // namespace mongo
