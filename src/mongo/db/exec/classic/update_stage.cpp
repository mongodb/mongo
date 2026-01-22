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

#include "mongo/db/exec/classic/update_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/match_details.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

/**
 * Returns true if we should throw a WriteConflictException in order to retry the operation in the
 * case of a conflict. Returns false if we should skip the document and keep going.
 */
bool shouldRestartUpdateIfNoLongerMatches(const UpdateStageParams& params) {
    // When we're doing an updateOne or findAndModify with a sort, the sort will have a limit of 1,
    // so it will not produce any more results even if there is another matching document. Throw a
    // WCE here so that these operations get another chance to find a matching document. The
    // updateOne and findAndModify commands should automatically retry if they get a WCE.
    return !params.request->getSort().isEmpty();
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
                         CollectionAcquisition collection,
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
                         CollectionAcquisition collection)
    : RequiresWritableCollectionStage(kStageType.data(), expCtx, collection),
      _params(params),
      _ws(ws),
      _doc(params.driver->getDocument()),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new update::RecordIdSet() : nullptr),
      _preWriteFilter(opCtx(), collection.nss()) {

    // Should the modifiers validate their embedded docs via storage_validation::scanDocument()?
    // Only user updates should be checked. Any system or replication stuff should pass through.
    const auto request = _params.request;

    _isUserInitiatedWrite = opCtx()->writesAreReplicated() &&
        !(request->isFromOplogApplication() ||
          params.driver->type() == UpdateDriver::UpdateType::kDelta ||
          request->source() == OperationSource::kFromMigrate);

    _specificStats.isModUpdate = params.driver->type() == UpdateDriver::UpdateType::kOperator;
}

bool UpdateStage::isEOF() const {
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
    if (collectionPtr()->ns().isImplicitlyReplicated() && !_isUserInitiatedWrite) {
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
        const auto ensureStillMatchesRet = handlePlanStageYield(
            expCtx(),
            "UpdateStage ensureStillMatches",
            [&] {
                docStillMatches = write_stage_common::ensureStillMatches(
                    collectionPtr(), opCtx(), _ws, id, _params.canonicalQuery);
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
            auto [immediateReturnStageState, fromMigrate] = _preWriteFilter.checkIfNotWritable(
                member->doc.value(),
                "update"_sd,
                collectionPtr()->ns(),
                [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                    planExecutorShardingState(opCtx()).criticalSectionFuture =
                        ex->getCriticalSectionSignal();
                    memberFreer.dismiss();  // Keep this member around so we can retry updating it.
                    prepareToRetryWSM(id, out);
                });
            if (immediateReturnStageState) {
                return *immediateReturnStageState;
            }
            writeToOrphan = fromMigrate;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState()
        // is allowed to free the memory.
        member->makeObjOwnedIfNeeded();
        BSONObj oldObj = member->doc.value().toBson();
        invariant(oldObj.isOwned());

        const auto saveRet = handlePlanStageYield(
            expCtx(),
            "UpdateStage saveState",
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

        // If we care about the pre-updated version of the doc, save it out here.
        SnapshotId oldSnapshot = member->doc.snapshotId();

        BSONObj newObj;
        bool docWasModified;

        try {
            const auto updateRet = handlePlanStageYield(
                expCtx(),
                "UpdateStage update",
                [&] {
                    // Do the update, get us the new version of the doc.
                    std::tie(newObj, docWasModified) =
                        update::transformDocument(opCtx(),
                                                  collectionAcquisition(),
                                                  {oldSnapshot, oldObj},
                                                  _doc,
                                                  _isUserInitiatedWrite,
                                                  _params.canonicalQuery,
                                                  recordId,
                                                  _params.driver,
                                                  _params.request,
                                                  writeToOrphan,
                                                  _updatedRecordIds.get(),
                                                  nullptr /* cursor */);
                    tassert(11533704, "Transformed doc cannot be empty", !newObj.isEmpty());
                    if (docWasModified) {
                        // Only record doc modifications if they wrote (exclude no-ops). Explains
                        // get recorded as if they wrote.
                        _specificStats.nModified +=
                            _params.numStatsForDoc ? _params.numStatsForDoc(newObj) : 1;
                    }
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
            if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
                ex->getCriticalSectionSignal()) {
                // If the placement version is IGNORED and we encountered a critical section, then
                // yield, wait for critical section to finish and then we'll resume the write from
                // the point we had left. We do this to prevent large multi-writes from repeatedly
                // failing due to StaleConfig and exhausting the mongos retry attempts.
                planExecutorShardingState(opCtx()).criticalSectionFuture =
                    ex->getCriticalSectionSignal();
                memberFreer.dismiss();  // Keep this member around so we can retry updating it.
                prepareToRetryWSM(id, out);
                return PlanStage::NEED_YIELD;
            }
            throw;
        }

        // Set member's obj to be the doc we want to return.
        if (_params.request->shouldReturnAnyDocs()) {
            if (_params.request->shouldReturnNewDocs()) {
                member->resetDocument(shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId(),
                                      newObj);
            } else {
                invariant(_params.request->shouldReturnOldDocs());
                member->resetDocument(oldSnapshot, oldObj);
            }
            member->recordId = RecordId();
            member->transitionToOwnedObj();
        }

        // This should be after transformDocument to make sure we actually updated this doc.
        _specificStats.nMatched += _params.numStatsForDoc ? _params.numStatsForDoc(newObj) : 1;

        // Don't restore stage if we do an update with IDHACK. Saves us the work of opening a new
        // Wuow that we will never use
        if (child()->stageType() != STAGE_IDHACK) {
            // Restore state after modification. As restoreState may restore (recreate) cursors,
            // make sure to restore the state outside of the WritUnitOfWork.
            const auto stageIsEOF = isEOF();
            const auto restoreStateRet = handlePlanStageYield(
                expCtx(),
                "UpdateStage restoreState",
                [&] {
                    child()->restoreState(&collectionPtr());
                    return PlanStage::NEED_TIME;
                },
                [&] {
                    // yieldHandler
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
                });

            if (restoreStateRet != PlanStage::NEED_TIME) {
                if (restoreStateRet == PlanStage::NEED_YIELD && stageIsEOF &&
                    !shard_role_details::getLocker(opCtx())->inAWriteUnitOfWork()) {
                    // If this stage is already exhausted it won't use its children stages anymore
                    // and therefore it's okay if we failed to restore them. Avoid requesting a
                    // yield to the plan executor. Restoring from yield could fail due to a sharding
                    // placement change. Throwing a StaleConfig error is undesirable after an
                    // "update one" operation has already performed a write because the router would
                    // retry. Unset _idReturning as we'll return the document in this stage
                    // iteration.
                    //
                    // If this plan is part of a larger encompassing WUOW it would be illegal to
                    // skip returning NEED_YIELD, so we don't skip it. In this case, such as
                    // multi-doc transactions, this is okay as the PlanExecutor is not allowed to
                    // auto-yield.
                    _idReturning = WorkingSet::INVALID_ID;
                } else {
                    return restoreStateRet;
                }
            }
        }

        if (_params.request->shouldReturnAnyDocs()) {
            // member->obj should refer to the document we want to return.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            memberFreer.dismiss();  // Keep this member around so we can return it.
            *out = id;
            return PlanStage::ADVANCED;
        }

        return isEOF() ? PlanStage::IS_EOF : PlanStage::NEED_TIME;
    } else if (PlanStage::IS_EOF == status) {
        // The child is out of results, and therefore so are we.
        return PlanStage::IS_EOF;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
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
                                << nsString.toStringForErrorMsg());
    }

    // Single updates never yield after having already modified one document. Otherwise restore
    // could fail (e.g. due to a sharding placement change) and we'd fail to report in the response
    // the already modified documents.
    const bool singleUpdateAndAlreadyWrote = !_params.request->isMulti() &&
        (_specificStats.nModified > 0 || _specificStats.nUpserted > 0);
    tassert(7711601,
            "Single update should never restore after having already modified one document.",
            !singleUpdateAndAlreadyWrote || request.explain());

    _preWriteFilter.restoreState();
}

std::unique_ptr<PlanStageStats> UpdateStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_UPDATE);
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

}  // namespace mongo
