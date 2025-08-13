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


#include "mongo/db/exec/classic/delete_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

using std::unique_ptr;

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
                         CollectionAcquisition collection,
                         PlanStage* child)
    : DeleteStage(kStageType.data(), expCtx, std::move(params), ws, collection, child) {}

DeleteStage::DeleteStage(const char* stageType,
                         ExpressionContext* expCtx,
                         std::unique_ptr<DeleteStageParams> params,
                         WorkingSet* ws,
                         CollectionAcquisition collection,
                         PlanStage* child)
    : RequiresWritableCollectionStage(stageType, expCtx, collection),
      _params(std::move(params)),
      _ws(ws),
      _preWriteFilter(opCtx(), collection.nss()),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID) {
    _children.emplace_back(child);
}

bool DeleteStage::isEOF() const {
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

    const auto ret = handlePlanStageYield(
        expCtx(),
        "DeleteStage ensureStillMatches",
        [&] {
            docStillMatches = write_stage_common::ensureStillMatches(
                collectionPtr(), opCtx(), _ws, id, _params->canonicalQuery);
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
        auto [immediateReturnStageState, fromMigrate] = _preWriteFilter.checkIfNotWritable(
            member->doc.value(),
            "delete"_sd,
            collectionPtr()->ns(),
            [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                planExecutorShardingState(opCtx()).criticalSectionFuture =
                    ex->getCriticalSectionSignal();
                memberFreer.dismiss();  // Keep this member around so we can retry deleting it.
                prepareToRetryWSM(id, out);
            });
        if (immediateReturnStageState) {
            return *immediateReturnStageState;
        }
        writeToOrphan = fromMigrate;
    }

    auto retryableWrite = write_stage_common::isRetryableWrite(opCtx());

    // Ensure that the BSONObj underlying the WSM is owned because saveState() is
    // allowed to free the memory the BSONObj points to. The BSONObj will be needed
    // later when it is passed to collection_internal::deleteDocument(). Note that the call to
    // makeObjOwnedIfNeeded() will leave the WSM in the RID_AND_OBJ state in case we need to retry
    // deleting it.
    member->makeObjOwnedIfNeeded();

    Snapshotted<Document> memberDoc = member->doc;
    BSONObj bsonObjDoc = memberDoc.value().toBson();

    const auto saveRet = handlePlanStageYield(
        expCtx(),
        "DeleteStage saveState",
        [&] {
            child()->saveState();
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            memberFreer.dismiss();
            prepareToRetryWSM(id, out);
        });
    if (saveRet != PlanStage::NEED_TIME) {
        return saveRet;
    }

    // Do the write, unless this is an explain.
    if (!_params->isExplain) {
        try {
            const auto ret = handlePlanStageYield(
                expCtx(),
                "DeleteStage deleteDocument",
                [&] {
                    WriteUnitOfWork wunit(opCtx());
                    collection_internal::deleteDocument(
                        opCtx(),
                        collectionPtr(),
                        Snapshotted(memberDoc.snapshotId(), bsonObjDoc),
                        _params->stmtId,
                        recordId,
                        _params->opDebug,
                        writeToOrphan || _params->fromMigrate,
                        false,
                        _params->returnDeleted ? collection_internal::StoreDeletedDoc::On
                                               : collection_internal::StoreDeletedDoc::Off,
                        CheckRecordId::Off,
                        retryableWrite ? collection_internal::RetryableWrite::kYes
                                       : collection_internal::RetryableWrite::kNo);
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
            if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
                ex->getCriticalSectionSignal()) {
                // If the placement version is IGNORED and we encountered a critical section, then
                // yield, wait for the critical section to finish and then we'll resume the write
                // from the point we had left. We do this to prevent large multi-writes from
                // repeatedly failing due to StaleConfig and exhausting the mongos retry attempts.
                planExecutorShardingState(opCtx()).criticalSectionFuture =
                    ex->getCriticalSectionSignal();
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

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in which
    // they are created, and a WriteUnitOfWork is a transaction, make sure to restore the state
    // outside of the WriteUnitOfWork.
    const auto stageIsEOF = isEOF();
    const auto restoreStateRet = handlePlanStageYield(
        expCtx(),
        "DeleteStage restoreState",
        [&] {
            child()->restoreState(&collectionPtr());
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            // Note we don't need to retry anything in this case since the delete already was
            // committed. However, we still need to return the deleted document (if it was
            // requested).
            if (_params->returnDeleted) {
                // member->obj should refer to the deleted document.
                invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

                _idReturning = id;
                // Keep this member around so that we can return it on the next work() call.
                memberFreer.dismiss();
            }
            *out = WorkingSet::INVALID_ID;
        });

    if (restoreStateRet != PlanStage::NEED_TIME) {
        if (restoreStateRet == PlanStage::NEED_YIELD && stageIsEOF &&
            !shard_role_details::getLocker(opCtx())->inAWriteUnitOfWork()) {
            // If this stage is already exhausted it won't use its children stages anymore and
            // therefore it's okay if we failed to restore them. Avoid requesting a yield to the
            // plan executor. Restoring from yield could fail due to a sharding placement change.
            // Throwing a StaleConfig error is undesirable after an "delete one" operation has
            // already performed a write because the router would retry. Unset _idReturning as we'll
            // return the document in this stage iteration.
            //
            // If this plan is part of a larger encompassing WUOW it would be illegal to skip
            // returning NEED_YIELD, so we don't skip it. In this case, such as multi-doc
            // transactions, this is okay as the PlanExecutor is not allowed to auto-yield.
            _idReturning = WorkingSet::INVALID_ID;
        } else {
            return restoreStateRet;
        }
    }

    if (_params->returnDeleted) {
        // member->obj should refer to the deleted document.
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        memberFreer.dismiss();  // Keep this member around so we can return it.
        *out = id;
        return PlanStage::ADVANCED;
    }

    return isEOF() ? PlanStage::IS_EOF : PlanStage::NEED_TIME;
}

void DeleteStage::doRestoreStateRequiresCollection() {
    const NamespaceString& ns = collectionPtr()->ns();
    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Demoted from primary while removing from "
                          << ns.toStringForErrorMsg(),
            !opCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), ns));

    // Single deletes never yield after having already deleted one document. Otherwise restore could
    // fail (e.g. due to a sharding placement change) and we'd fail to report in the response the
    // already deleted documents.
    const bool singleDeleteAndAlreadyDeleted = !_params->isMulti && _specificStats.docsDeleted > 0;
    tassert(7711600,
            "Single delete should never restore after having already deleted one document.",
            !singleDeleteAndAlreadyDeleted || _params->isExplain);

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
