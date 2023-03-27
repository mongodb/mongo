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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/rename_collection_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/recoverable_critical_section_service.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

boost::optional<CollectionType> getShardedCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    try {
        return Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist
        return boost::none;
    }
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        NamespaceString const& nss,
                                        boost::optional<CollectionType> const& optCollectionType,
                                        bool throwOnNotFound = true) {
    if (optCollectionType) {
        return optCollectionType->getUuid();
    }
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    const auto collPtr = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    if (!collPtr && !throwOnNotFound) {
        return boost::none;
    }

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss << " doesn't exist.",
            collPtr);

    return collPtr->uuid();
}
}  // namespace

RenameCollectionCoordinator::RenameCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                         const BSONObj& initialState)
    : ShardingDDLCoordinator(service, initialState),
      _doc(RenameCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("RenameCollectionCoordinatorDocument"), initialState)),
      _request(_doc.getRenameCollectionRequest()) {}

void RenameCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = RenameCollectionCoordinatorDocument::parse(
        IDLParserErrorContext("RenameCollectionCoordinatorDocument"), doc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another rename collection for namespace " << nss()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

std::vector<StringData> RenameCollectionCoordinator::_acquireAdditionalLocks(
    OperationContext* opCtx) {
    return {_request.getTo().ns()};
}

boost::optional<BSONObj> RenameCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.appendElements(_request.toBSON());

    const auto currPhase = [&]() {
        stdx::lock_guard l{_docMutex};
        return _doc.getPhase();
    }();

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "RenameCollectionCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", currPhase);
    bob.append("active", true);
    return bob.obj();
}

void RenameCollectionCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5460501,
                2,
                "Rename collection coordinator phase transition",
                "fromNs"_attr = nss(),
                "toNs"_attr = _request.getTo(),
                "newPhase"_attr = RenameCollectionCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = RenameCollectionCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        newDoc = _insertStateDocument(std::move(newDoc));
    } else {
        newDoc = _updateStateDocument(cc().makeOperationContext().get(), std::move(newDoc));
    }

    {
        stdx::unique_lock ul{_docMutex};
        _doc = std::move(newDoc);
    }
}

ExecutorFuture<void> RenameCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kCheckPreconditions,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                const auto criticalSectionReason =
                    sharding_ddl_util::getCriticalSectionReasonForRename(fromNss, toNss);

                try {
                    uassert(ErrorCodes::InvalidOptions,
                            "Cannot provide an expected collection UUID when renaming between "
                            "databases",
                            fromNss.db() == toNss.db() ||
                                (!_doc.getExpectedSourceUUID() && !_doc.getExpectedTargetUUID()));

                    {
                        AutoGetCollection coll{
                            opCtx, fromNss, MODE_IS, AutoGetCollectionViewMode::kViewsPermitted};
                        checkCollectionUUIDMismatch(
                            opCtx, fromNss, *coll, _doc.getExpectedSourceUUID());

                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename an encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                    // Make sure the source collection exists
                    const auto optSourceCollType = getShardedCollection(opCtx, fromNss);
                    const bool sourceIsSharded = (bool)optSourceCollType;

                    _doc.setSourceUUID(getCollectionUUID(opCtx, fromNss, optSourceCollType));
                    if (sourceIsSharded) {
                        uassert(ErrorCodes::CommandFailed,
                                str::stream() << "Source and destination collections must be on "
                                                 "the same database because "
                                              << fromNss << " is sharded.",
                                fromNss.db() == toNss.db());
                        _doc.setOptShardedCollInfo(optSourceCollType);
                    } else if (fromNss.db() != toNss.db()) {
                        sharding_ddl_util::checkDbPrimariesOnTheSameShard(opCtx, fromNss, toNss);
                    }

                    const auto optTargetCollType = getShardedCollection(opCtx, toNss);
                    const bool targetIsSharded = (bool)optTargetCollType;
                    _doc.setTargetIsSharded(targetIsSharded);
                    _doc.setTargetUUID(getCollectionUUID(
                        opCtx, toNss, optTargetCollType, /*throwNotFound*/ false));

                    if (!targetIsSharded) {
                        // (SERVER-67325) Acquire critical section on the target collection in order
                        // to disallow concurrent `createCollection`. In case the collection does
                        // not exist, it will be later released by the rename participant. In case
                        // the collection exists and is unsharded, the critical section can be
                        // released right away as the participant will re-acquire it when needed.
                        auto criticalSection = RecoverableCriticalSectionService::get(opCtx);
                        criticalSection->acquireRecoverableCriticalSectionBlockWrites(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                        criticalSection->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);

                        // Make sure the target namespace is not a view
                        uassert(ErrorCodes::CommandNotSupportedOnView,
                                str::stream() << "Can't rename to target collection `" << toNss
                                              << "` because it is a view.",
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, toNss));

                        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                                       toNss)) {
                            // Release the critical section because the unsharded target collection
                            // already exists, hence no risk of concurrent `createCollection`
                            criticalSection->releaseRecoverableCriticalSection(
                                opCtx,
                                toNss,
                                criticalSectionReason,
                                WriteConcerns::kLocalWriteConcern);
                        }
                    }

                    sharding_ddl_util::checkRenamePreconditions(
                        opCtx, sourceIsSharded, toNss, _doc.getDropTarget());

                    sharding_ddl_util::checkCatalogConsistencyAcrossShardsForRename(
                        opCtx, fromNss, toNss, _doc.getDropTarget(), executor);

                    {
                        AutoGetCollection coll{opCtx, toNss, MODE_IS};
                        checkCollectionUUIDMismatch(
                            opCtx, toNss, *coll, _doc.getExpectedTargetUUID());
                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename to an existing encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                } catch (const DBException&) {
                    auto criticalSection = RecoverableCriticalSectionService::get(opCtx);
                    criticalSection->releaseRecoverableCriticalSection(
                        opCtx,
                        toNss,
                        criticalSectionReason,
                        WriteConcerns::kLocalWriteConcern,
                        false /* throwIfReasonDiffers */);
                    _completeOnError = true;
                    throw;
                }
            }))
        .then(_executePhase(
            Phase::kFreezeMigrations,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.start",
                    fromNss.ns(),
                    BSON("source" << fromNss.toString() << "destination" << toNss.toString()),
                    ShardingCatalogClient::kMajorityWriteConcern);

                // Block migrations on involved sharded collections
                if (_doc.getOptShardedCollInfo()) {
                    sharding_ddl_util::stopMigrations(opCtx, fromNss, _doc.getSourceUUID());
                }

                if (_doc.getTargetIsSharded()) {
                    sharding_ddl_util::stopMigrations(opCtx, toNss, _doc.getTargetUUID());
                }
            }))
        .then(_executePhase(
            Phase::kBlockCrudAndRename,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _doc = _updateSession(opCtx, _doc);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(_doc), **executor);
                }

                const auto& fromNss = nss();

                _doc = _updateSession(opCtx, _doc);
                const OperationSessionInfo osi = getCurrentSession(_doc);

                // On participant shards:
                // - Block CRUD on source and target collection in case at least one
                // of such collections is currently sharded.
                // - Locally drop the target collection
                // - Locally rename source to target
                ShardsvrRenameCollectionParticipant renameCollParticipantRequest(
                    fromNss, _doc.getSourceUUID().get());
                renameCollParticipantRequest.setDbName(fromNss.db());
                renameCollParticipantRequest.setTargetUUID(_doc.getTargetUUID());
                renameCollParticipantRequest.setRenameCollectionRequest(_request);

                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                // We need to send the command to all the shards because both
                // movePrimary and moveChunk leave garbage behind for sharded
                // collections.
                const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                    renameCollParticipantRequest.toBSON({}));

                try {
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx,
                        fromNss.db(),
                        cmdObj.addFields(osi.toBSON()),
                        participants,
                        **executor);

                } catch (const ExceptionFor<ErrorCodes::NotARetryableWriteCommand>&) {
                    // Older 5.0 binaries don't support running the command as a
                    // retryable write yet. In that case, retry without attaching session info.
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx, fromNss.db(), cmdObj, participants, **executor);
                }
            }))
        .then(_executePhase(
            Phase::kRenameMetadata,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _doc = _updateSession(opCtx, _doc);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(_doc), **executor);
                }

                ConfigsvrRenameCollectionMetadata req(nss(), _request.getTo());
                req.setOptFromCollection(_doc.getOptShardedCollInfo());
                const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(req.toBSON({}));
                const auto& configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                // For an unsharded collection the CSRS server can not verify the targetUUID.
                // Use the session ID + txnNumber to ensure no stale requests get through.
                _doc = _updateSession(opCtx, _doc);
                const OperationSessionInfo osi = getCurrentSession(_doc);

                try {
                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(
                        configShard->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                "admin",
                                                cmdObj.addFields(osi.toBSON()),
                                                Shard::RetryPolicy::kIdempotent)));
                } catch (const ExceptionFor<ErrorCodes::NotARetryableWriteCommand>&) {
                    // Older 5.0 binaries don't support running the command as a
                    // retryable write yet. In that case, retry without attaching session info.
                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(
                        configShard->runCommand(opCtx,
                                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                                "admin",
                                                cmdObj,
                                                Shard::RetryPolicy::kIdempotent)));
                }
            }))
        .then(_executePhase(
            Phase::kUnblockCRUD,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _doc = _updateSession(opCtx, _doc);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(_doc), **executor);
                }

                const auto& fromNss = nss();
                // On participant shards:
                // - Unblock CRUD on participants for both source and destination collections
                ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(
                    fromNss, _doc.getSourceUUID().get());
                unblockParticipantRequest.setDbName(fromNss.db());
                unblockParticipantRequest.setRenameCollectionRequest(_request);
                auto const cmdObj = CommandHelpers::appendMajorityWriteConcern(
                    unblockParticipantRequest.toBSON({}));
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

                _doc = _updateSession(opCtx, _doc);
                const OperationSessionInfo osi = getCurrentSession(_doc);

                try {
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx,
                        fromNss.db(),
                        cmdObj.addFields(osi.toBSON()),
                        participants,
                        **executor);
                } catch (const ExceptionFor<ErrorCodes::NotARetryableWriteCommand>&) {
                    // Older 5.0 binaries don't support running the command as a
                    // retryable write yet. In that case, retry without attaching session info.
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx, fromNss.db(), cmdObj, participants, **executor);
                }
            }))
        .then(_executePhase(Phase::kSetResponse,
                            [this, anchor = shared_from_this()] {
                                auto opCtxHolder = cc().makeOperationContext();
                                auto* opCtx = opCtxHolder.get();
                                getForwardableOpMetadata().setOn(opCtx);

                                // Retrieve the new collection version
                                const auto catalog = Grid::get(opCtx)->catalogCache();
                                const auto cm =
                                    uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(
                                        opCtx, _request.getTo()));
                                _response = RenameCollectionResponse(
                                    cm.isSharded() ? cm.getVersion() : ChunkVersion::UNSHARDED());

                                ShardingLogging::get(opCtx)->logChange(
                                    opCtx,
                                    "renameCollection.end",
                                    nss().ns(),
                                    BSON("source" << nss().toString() << "destination"
                                                  << _request.getTo().toString()),
                                    ShardingCatalogClient::kMajorityWriteConcern);
                                LOGV2(5460504, "Collection renamed", "namespace"_attr = nss());
                            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5460505,
                            "Error running rename collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
            }

            return status;
        });
}

}  // namespace mongo
