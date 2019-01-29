
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

#include "mongo/db/exec/delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

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

// static
const char* DeleteStage::kStageType = "DELETE";

DeleteStage::DeleteStage(OperationContext* opCtx,
                         std::unique_ptr<DeleteStageParams> params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : RequiresMutableCollectionStage(kStageType, opCtx, collection),
      _params(std::move(params)),
      _ws(ws),
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

            case PlanStage::FAILURE:
                // The stage which produces a failure is responsible for allocating a working set
                // member with error details.
                invariant(WorkingSet::INVALID_ID != id);
                *out = id;
                return status;

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
    auto memberFreer = makeGuard([&] { _ws->free(id); });

    invariant(member->hasRecordId());
    RecordId recordId = member->recordId;
    // Deletes can't have projections. This means that covering analysis will always add
    // a fetch. We should always get fetched data, and never just key data.
    invariant(member->hasObj());

    // Ensure the document still exists and matches the predicate.
    bool docStillMatches;
    try {
        docStillMatches = write_stage_common::ensureStillMatches(
            collection(), getOpCtx(), _ws, id, _params->canonicalQuery);
    } catch (const WriteConflictException&) {
        // There was a problem trying to detect if the document still exists, so retry.
        memberFreer.dismiss();
        return prepareToRetryWSM(id, out);
    }

    if (!docStillMatches) {
        // Either the document has already been deleted, or it has been updated such that it no
        // longer matches the predicate.
        if (shouldRestartDeleteIfNoLongerMatches(_params.get())) {
            throw WriteConflictException();
        }
        return PlanStage::NEED_TIME;
    }

    // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState() is
    // allowed to free the memory.
    if (_params->returnDeleted) {
        // Save a copy of the document that is about to get deleted, but keep it in the RID_AND_OBJ
        // state in case we need to retry deleting it.
        BSONObj deletedDoc = member->obj.value();
        member->obj.setValue(deletedDoc.getOwned());
    }

    if (_params->removeSaver) {
        uassertStatusOK(_params->removeSaver->goingToDelete(member->obj.value()));
    }

    // TODO: Do we want to buffer docs and delete them in a group rather than saving/restoring state
    // repeatedly?

    WorkingSetCommon::prepareForSnapshotChange(_ws);
    try {
        child()->saveState();
    } catch (const WriteConflictException&) {
        std::terminate();
    }

    // Do the write, unless this is an explain.
    if (!_params->isExplain) {
        try {
            WriteUnitOfWork wunit(getOpCtx());
            collection()->deleteDocument(getOpCtx(),
                                         _params->stmtId,
                                         recordId,
                                         _params->opDebug,
                                         _params->fromMigrate,
                                         false,
                                         _params->returnDeleted ? Collection::StoreDeletedDoc::On
                                                                : Collection::StoreDeletedDoc::Off);
            wunit.commit();
        } catch (const WriteConflictException&) {
            memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
            return prepareToRetryWSM(id, out);
        }
    }
    ++_specificStats.docsDeleted;

    if (_params->returnDeleted) {
        // After deleting the document, the RecordId associated with this member is invalid.
        // Remove the 'recordId' from the WorkingSetMember before returning it.
        member->recordId = RecordId();
        member->transitionToOwnedObj();
    }

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in which
    // they are created, and a WriteUnitOfWork is a transaction, make sure to restore the state
    // outside of the WriteUnitOfWork.
    try {
        child()->restoreState();
    } catch (const WriteConflictException&) {
        // Note we don't need to retry anything in this case since the delete already was committed.
        // However, we still need to return the deleted document (if it was requested).
        if (_params->returnDeleted) {
            // member->obj should refer to the deleted document.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            _idReturning = id;
            // Keep this member around so that we can return it on the next work() call.
            memberFreer.dismiss();
        }
        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
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
            !getOpCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(getOpCtx())->canAcceptWritesFor(getOpCtx(), ns));
}

unique_ptr<PlanStageStats> DeleteStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_DELETE);
    ret->specific = make_unique<DeleteStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* DeleteStage::getSpecificStats() const {
    return &_specificStats;
}

// static
long long DeleteStage::getNumDeleted(const PlanExecutor& exec) {
    invariant(exec.getRootStage()->isEOF());

    // If we're deleting from a non-existent collection, then the delete plan may have an EOF as the
    // root stage.
    if (exec.getRootStage()->stageType() == STAGE_EOF) {
        return 0LL;
    }

    // If the collection exists, the delete plan may either have a delete stage at the root, or (for
    // findAndModify) a projection stage wrapping a delete stage.
    switch (exec.getRootStage()->stageType()) {
        case StageType::STAGE_PROJECTION_DEFAULT:
        case StageType::STAGE_PROJECTION_COVERED:
        case StageType::STAGE_PROJECTION_SIMPLE: {
            invariant(exec.getRootStage()->getChildren().size() == 1U);
            invariant(StageType::STAGE_DELETE == exec.getRootStage()->child()->stageType());
            const SpecificStats* stats = exec.getRootStage()->child()->getSpecificStats();
            return static_cast<const DeleteStats*>(stats)->docsDeleted;
        }
        default: {
            invariant(StageType::STAGE_DELETE == exec.getRootStage()->stageType());
            const auto* deleteStats =
                static_cast<const DeleteStats*>(exec.getRootStage()->getSpecificStats());
            return deleteStats->docsDeleted;
        }
    }
}

PlanStage::StageState DeleteStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

}  // namespace mongo
