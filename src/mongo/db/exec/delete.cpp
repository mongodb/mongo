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

#include "mongo/db/exec/delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator_global.h"
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
bool shouldRestartDeleteIfNoLongerMatches(const DeleteStageParams& params) {
    // When we're doing a findAndModify with a sort, the sort will have a limit of 1, so it will not
    // produce any more results even if there is another matching document. Throw a WCE here so that
    // these operations get another chance to find a matching document. The findAndModify command
    // should automatically retry if it gets a WCE.
    return params.returnDeleted && !params.sort.isEmpty();
};

}  // namespace

// static
const char* DeleteStage::kStageType = "DELETE";

DeleteStage::DeleteStage(OperationContext* opCtx,
                         const DeleteStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : PlanStage(kStageType, opCtx),
      _params(params),
      _ws(ws),
      _collection(collection),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID) {
    _children.emplace_back(child);
}

bool DeleteStage::isEOF() {
    if (!_collection) {
        return true;
    }
    if (!_params.isMulti && _specificStats.docsDeleted > 0) {
        return true;
    }
    return _idRetrying == WorkingSet::INVALID_ID && _idReturning == WorkingSet::INVALID_ID &&
        child()->isEOF();
}

PlanStage::StageState DeleteStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }
    invariant(_collection);  // If isEOF() returns false, we must have a collection.

    // It is possible that after a delete was executed, a WriteConflictException occurred
    // and prevented us from returning ADVANCED with the old version of the document.
    if (_idReturning != WorkingSet::INVALID_ID) {
        // We should only get here if we were trying to return something before.
        invariant(_params.returnDeleted);

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
            case PlanStage::DEAD:
                *out = id;

                // If a stage fails, it may create a status WSM to indicate why it failed, in which
                // case 'id' is valid.  If ID is invalid, we create our own error message.
                if (WorkingSet::INVALID_ID == id) {
                    const std::string errmsg = "delete stage failed to read in results from child";
                    *out = WorkingSetCommon::allocateStatusMember(
                        _ws, Status(ErrorCodes::InternalError, errmsg));
                }
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
    ScopeGuard memberFreer = MakeGuard(&WorkingSet::free, _ws, id);

    if (!member->hasRecordId()) {
        // We expect to be here because of an invalidation causing a force-fetch.
        ++_specificStats.nInvalidateSkips;
        return PlanStage::NEED_TIME;
    }
    RecordId recordId = member->recordId;
    // Deletes can't have projections. This means that covering analysis will always add
    // a fetch. We should always get fetched data, and never just key data.
    invariant(member->hasObj());

    // Ensure the document still exists and matches the predicate.
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
        // Either the document has already been deleted, or it has been updated such that it no
        // longer matches the predicate.
        if (shouldRestartDeleteIfNoLongerMatches(_params)) {
            throw WriteConflictException();
        }
        return PlanStage::NEED_TIME;
    }

    // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState() is
    // allowed to free the memory.
    if (_params.returnDeleted) {
        // Save a copy of the document that is about to get deleted, but keep it in the RID_AND_OBJ
        // state in case we need to retry deleting it.
        BSONObj deletedDoc = member->obj.value();
        member->obj.setValue(deletedDoc.getOwned());
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
    if (!_params.isExplain) {
        try {
            WriteUnitOfWork wunit(getOpCtx());
            _collection->deleteDocument(
                getOpCtx(), _params.stmtId, recordId, _params.opDebug, _params.fromMigrate);
            wunit.commit();
        } catch (const WriteConflictException&) {
            memberFreer.Dismiss();  // Keep this member around so we can retry deleting it.
            return prepareToRetryWSM(id, out);
        }
    }
    ++_specificStats.docsDeleted;

    if (_params.returnDeleted) {
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
        if (_params.returnDeleted) {
            // member->obj should refer to the deleted document.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            _idReturning = id;
            // Keep this member around so that we can return it on the next work() call.
            memberFreer.Dismiss();
        }
        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
    }

    if (_params.returnDeleted) {
        // member->obj should refer to the deleted document.
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        memberFreer.Dismiss();  // Keep this member around so we can return it.
        *out = id;
        return PlanStage::ADVANCED;
    }

    return PlanStage::NEED_TIME;
}

void DeleteStage::doRestoreState() {
    invariant(_collection);
    const NamespaceString& ns(_collection->ns());
    uassert(28537,
            str::stream() << "Demoted from primary while removing from " << ns.ns(),
            !getOpCtx()->writesAreReplicated() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(getOpCtx(), ns));
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
    invariant(exec.getRootStage()->stageType() == STAGE_DELETE);
    DeleteStage* deleteStage = static_cast<DeleteStage*>(exec.getRootStage());
    const DeleteStats* deleteStats =
        static_cast<const DeleteStats*>(deleteStage->getSpecificStats());
    return deleteStats->docsDeleted;
}

PlanStage::StageState DeleteStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

}  // namespace mongo
