/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/rename_collection_participant_service.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <string>
#include <tuple>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/drop_collection_coordinator.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

/*
 * Drop the collection locally and clear stale metadata from cache collections.
 */
void dropCollectionLocally(OperationContext* opCtx,
                           const NamespaceString& nss,
                           bool markFromMigrate) {
    DropCollectionCoordinator::dropCollectionLocally(
        opCtx, nss, markFromMigrate, false /* dropSystemCollections */);
    LOGV2_DEBUG(5515100,
                1,
                "Dropped target collection locally on renameCollection participant.",
                logAttrs(nss));
}

/*
 * Rename the collection if exists locally, otherwise simply drop the target collection.
 */
void renameOrDropTarget(OperationContext* opCtx,
                        const NamespaceString& fromNss,
                        const NamespaceString& toNss,
                        const RenameCollectionOptions& options,
                        const UUID& sourceUUID,
                        const boost::optional<UUID>& targetUUID) {
    {
        Lock::DBLock dbLock(opCtx, toNss.dbName(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, toNss, MODE_IS);
        const auto targetCollPtr =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss);
        if (targetCollPtr) {
            if (targetCollPtr->uuid() == sourceUUID) {
                // Early return if the rename previously succeeded
                return;
            }
            uassert(5807602,
                    str::stream() << "Target collection " << toNss.toStringForErrorMsg()
                                  << " UUID does not match the provided UUID.",
                    !targetUUID || targetCollPtr->uuid() == *targetUUID);
        }
    }

    {
        Lock::DBLock dbLock(opCtx, fromNss.dbName(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, fromNss, MODE_IS);
        // ensure idempotency by checking sourceUUID
        const auto sourceCollPtr =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, fromNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Source Collection " << fromNss.toStringForErrorMsg()
                              << " UUID does not match provided uuid.",
                !sourceCollPtr || sourceCollPtr->uuid() == sourceUUID);
    }

    try {
        validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // It's ok for a participant shard to have no knowledge about a collection
        LOGV2_DEBUG(5515101,
                    1,
                    "Source namespace not found while trying to rename collection on participant",
                    logAttrs(fromNss));
        dropCollectionLocally(opCtx, toNss, options.markFromMigrate);
        rangedeletionutil::deleteRangeDeletionTasksForRename(opCtx, fromNss, toNss);
    }
}

void clearFilteringMetadataOnNss(OperationContext* opCtx, const NamespaceString& nss) {
    // Set the placement version to UNKNOWN to force a future operation to refresh the metadata
    // TODO (SERVER-71444): Fix to be interruptible or document exception.
    UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->clearFilteringMetadata(opCtx);
}

}  // namespace

RenameCollectionParticipantService* RenameCollectionParticipantService::getService(
    OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kServiceName);
    return checked_cast<RenameCollectionParticipantService*>(std::move(service));
}

std::shared_ptr<RenameCollectionParticipantService::Instance>
RenameCollectionParticipantService::constructInstance(BSONObj initialState) {
    LOGV2_DEBUG(5515102,
                2,
                "Constructing new rename participant",
                "renameParticipantDoc"_attr = initialState);
    return std::make_shared<RenameParticipantInstance>(std::move(initialState));
}

RenameParticipantInstance::~RenameParticipantInstance() {
    invariant(_unblockCRUDPromise.getFuture().isReady());
}

bool RenameParticipantInstance::hasSameOptions(const BSONObj& participantDoc) {
    const auto otherDoc = RenameCollectionParticipantDocument::parse(
        IDLParserContext("RenameCollectionParticipantDocument"), participantDoc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    return SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq);
}

boost::optional<BSONObj> RenameParticipantInstance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "RenameParticipantInstance");
    bob.append("op", "command");

    {
        stdx::lock_guard<Latch> lg(_stateMutex);
        bob.append("ns",
                   NamespaceStringUtil::serialize(_doc.getFromNss(),
                                                  SerializationContext::stateDefault()));
        bob.append(
            "to",
            NamespaceStringUtil::serialize(_doc.getTo(), SerializationContext::stateDefault()));
        BSONObjBuilder cmdBob;
        if (const auto& optComment = _doc.getForwardableOpMetadata().getComment()) {
            cmdBob.append(optComment.value().firstElement());
        }

        bob.append("command", cmdBob.obj());
        bob.append("currentPhase", _doc.getPhase());
    }

    bob.append("active", true);
    return bob.obj();
}

void RenameParticipantInstance::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5515104,
                2,
                "Rename participant phase transition",
                "fromNs"_attr = _doc.getFromNss(),
                "toNs"_attr = _doc.getTo(),
                "newPhase"_attr = RenameCollectionParticipantPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = RenameCollectionParticipantPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingRenameParticipantsNamespace);

    if (_doc.getPhase() == Phase::kUnset) {
        try {
            store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of step-up and step-down events can cause a node to try and insert the
            // document when it has already been persisted locally, but we must still wait for
            // majority commit.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
            const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajorityForWrite(opCtx->getServiceContext(),
                                           lastLocalOpTime,
                                           opCtx.get()->getCancellationToken())
                .get(opCtx.get());
        }
    } else {
        store.update(opCtx.get(),
                     BSON(StateDoc::kFromNssFieldName << NamespaceStringUtil::serialize(
                              _doc.getFromNss(), SerializationContext::stateDefault())),
                     newDoc.toBSON(),
                     WriteConcerns::kMajorityWriteConcernNoTimeout);
    }

    {
        stdx::lock_guard<Latch> lg(_stateMutex);
        _doc = std::move(newDoc);
    }
}

void RenameParticipantInstance::_removeStateDocument(OperationContext* opCtx) {
    LOGV2_DEBUG(5515105,
                2,
                "Removing state document for rename collection participant",
                "fromNs"_attr = _doc.getFromNss(),
                "toNs"_attr = _doc.getTo());

    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingRenameParticipantsNamespace);
    store.remove(opCtx,
                 BSON(StateDoc::kFromNssFieldName << NamespaceStringUtil::serialize(
                          _doc.getFromNss(), SerializationContext::stateDefault())),
                 WriteConcerns::kMajorityWriteConcernNoTimeout);
    {
        stdx::lock_guard<Latch> lg(_stateMutex);
        _doc = {};
    }
}


void RenameParticipantInstance::_invalidateFutures(const Status& errStatus, WithLock) {
    if (!_blockCRUDAndRenameCompletionPromise.getFuture().isReady()) {
        _blockCRUDAndRenameCompletionPromise.setError(errStatus);
    }

    if (!_canUnblockCRUDPromise.getFuture().isReady()) {
        _canUnblockCRUDPromise.setError(errStatus);
    }

    if (!_unblockCRUDPromise.getFuture().isReady()) {
        _unblockCRUDPromise.setError(errStatus);
    }
}

SemiFuture<void> RenameParticipantInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            return AsyncTry([this, executor, token] { return _runImpl(executor, token); })
                .until([this, token](Status status) { return status.isOK() || token.isCanceled(); })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isOK()) {
                // The token gets canceled in case of stepdown/shutdown
                stdx::lock_guard<Latch> lg(_stateMutex);
                _invalidateFutures(status, lg);
                return;
            }

            try {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _removeStateDocument(opCtx);
            } catch (DBException& ex) {
                LOGV2_WARNING(5515108,
                              "Failed to remove rename participant state document",
                              "error"_attr = redact(ex));
                ex.addContext("Failed to remove rename participant state document"_sd);
                stdx::lock_guard<Latch> lg(_stateMutex);
                if (!_unblockCRUDPromise.getFuture().isReady()) {
                    _unblockCRUDPromise.setError(ex.toStatus());
                }
                throw;
            }

            stdx::lock_guard<Latch> lg(_stateMutex);
            if (!_unblockCRUDPromise.getFuture().isReady()) {
                _unblockCRUDPromise.emplaceValue();
            }
        })
        .semi();
}

SemiFuture<void> RenameParticipantInstance::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kBlockCRUDAndSnapshotRangeDeletions,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                const auto& fromNss = _doc.getFromNss();
                const auto& toNss = _doc.getTo();

                // Acquire source/target critical sections
                const auto reason =
                    sharding_ddl_util::getCriticalSectionReasonForRename(fromNss, toNss);
                auto service = ShardingRecoveryService::get(opCtx);
                service->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, fromNss, reason, ShardingCatalogClient::kLocalWriteConcern);
                service->promoteRecoverableCriticalSectionToBlockAlsoReads(
                    opCtx, fromNss, reason, ShardingCatalogClient::kLocalWriteConcern);
                service->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, toNss, reason, ShardingCatalogClient::kLocalWriteConcern);
                service->promoteRecoverableCriticalSectionToBlockAlsoReads(
                    opCtx, toNss, reason, ShardingCatalogClient::kLocalWriteConcern);

                // Clear the filtering metadata before releasing the critical section (it will be
                // recovered the next time is accessed) and to safely create new range deletion
                // tasks (the submission will serialize on the renamed collection's metadata
                // refresh).
                {
                    Lock::DBLock dbLock(opCtx, fromNss.dbName(), MODE_IX);
                    Lock::CollectionLock collLock(opCtx, fromNss, MODE_IX);
                    auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                            opCtx, fromNss);
                    scopedCsr->clearFilteringMetadataForDroppedCollection(opCtx);
                }

                {
                    Lock::DBLock dbLock(opCtx, toNss.dbName(), MODE_IX);
                    Lock::CollectionLock collLock(opCtx, toNss, MODE_IX);
                    auto scopedCsr =
                        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                             toNss);
                    scopedCsr->clearFilteringMetadata(opCtx);
                }

                rangedeletionutil::snapshotRangeDeletionsForRename(opCtx, fromNss, toNss);
            }))
        .then(_buildPhaseHandler(
            Phase::kRenameLocalAndRestoreRangeDeletions,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _doc.getForwardableOpMetadata().setOn(opCtx);
                const auto& fromNss = _doc.getFromNss();
                const auto& toNss = _doc.getTo();

                // TODO SERVER-74719 replace with a query to config.system.sharding_ddl_coordinators
                const auto primaryShardId =
                    Grid::get(opCtx)
                        ->catalogClient()
                        ->getDatabase(
                            opCtx, fromNss.dbName(), repl::ReadConcernLevel::kMajorityReadConcern)
                        .getPrimary();
                const auto thisShardId = ShardingState::get(opCtx)->shardId();

                RenameCollectionOptions options;
                options.dropTarget = _doc.getDropTarget();
                options.stayTemp = _doc.getStayTemp();
                options.newTargetCollectionUuid = _doc.getNewTargetCollectionUuid();
                // Use the "markFromMigrate" option so that change streams capturing events about
                // fromNss/toNss won't receive duplicate drop notifications.
                options.markFromMigrate = (thisShardId != primaryShardId);

                renameOrDropTarget(
                    opCtx, fromNss, toNss, options, _doc.getSourceUUID(), _doc.getTargetUUID());

                rangedeletionutil::restoreRangeDeletionTasksForRename(opCtx, toNss);
            }))
        .then(_buildPhaseHandler(
            Phase::kDeleteFromRangeDeletions,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                const auto& fromNss = _doc.getFromNss();
                const auto& toNss = _doc.getTo();

                rangedeletionutil::deleteRangeDeletionTasksForRename(opCtx, fromNss, toNss);

                {
                    stdx::lock_guard<Latch> lg(_stateMutex);
                    if (!_blockCRUDAndRenameCompletionPromise.getFuture().isReady()) {
                        _blockCRUDAndRenameCompletionPromise.setFrom(Status::OK());
                    }
                }

                LOGV2(5515106,
                      "Collection locally renamed, waiting for CRUD to be unblocked",
                      "fromNs"_attr = fromNss,
                      "toNs"_attr = toNss);
            }))
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kUnblockCRUD) {
                return _canUnblockCRUDPromise.getFuture();
            }

            // Checkpoint the vector clock to ensure causality in the event of a crash or shutdown.
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

            return SemiFuture<void>::makeReady().share();
        })
        .then(_buildPhaseHandler(
            Phase::kUnblockCRUD,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                const auto& fromNss = _doc.getFromNss();
                const auto& toNss = _doc.getTo();

                clearFilteringMetadataOnNss(opCtx, fromNss);
                clearFilteringMetadataOnNss(opCtx, toNss);

                // Release source/target critical sections
                // Note: Use 'throwIfReasonDiffers=false' on the destination collection because as
                // soon as the critical section is released migrations can start. In case this phase
                // needs to be retried, we could then encounter a critical section related to a
                // migration. It is not needed for the source collection because no migration can
                // start until it first becomes sharded, which cannot happen until the DDLLock is
                // released.
                const auto reason = BSON("command"
                                         << "rename"
                                         << "from"
                                         << NamespaceStringUtil::serialize(
                                                fromNss, SerializationContext::stateDefault())
                                         << "to"
                                         << NamespaceStringUtil::serialize(
                                                toNss, SerializationContext::stateDefault()));
                auto service = ShardingRecoveryService::get(opCtx);
                service->releaseRecoverableCriticalSection(
                    opCtx, fromNss, reason, ShardingCatalogClient::kLocalWriteConcern);
                service->releaseRecoverableCriticalSection(
                    opCtx, toNss, reason, WriteConcerns::kMajorityWriteConcernNoTimeout, false);

                LOGV2(5515107, "CRUD unblocked", "fromNs"_attr = fromNss, "toNs"_attr = toNss);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(6372200,
                        "Error executing rename collection participant. Going to be retried.",
                        "fromNs"_attr = _doc.getFromNss(),
                        "toNs"_attr = _doc.getTo(),
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

void RenameParticipantInstance::interrupt(Status status) noexcept {
    LOGV2_DEBUG(5515110,
                2,
                "Interrupt while running rename collection on participant",
                "fromNs"_attr = _doc.getFromNss(),
                "toNs"_attr = _doc.getTo(),
                "error"_attr = redact(status));

    invariant(status.isA<ErrorCategory::NotPrimaryError>() ||
              status.isA<ErrorCategory::ShutdownError>());

    stdx::lock_guard<Latch> lg(_stateMutex);
    _invalidateFutures(status, lg);
}

}  // namespace mongo
