/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/untrack_unsplittable_collection_coordinator.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void UntrackUnsplittableCollectionCoordinator::appendCommandInfo(
    BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(
        BSON("op" << "untrackUnsplittableCollection"
                  << "nss"
                  << NamespaceStringUtil::serialize(nss(), SerializationContext::stateDefault())));
}

void UntrackUnsplittableCollectionCoordinator::checkIfOptionsConflict(
    const BSONObj& coorDoc) const {
    stdx::lock_guard lk{_docMutex};
    const auto otherDoc = UntrackUnsplittableCollectionCoordinatorDocument::parse(
        coorDoc, IDLParserContext("UntrackUnsplittableCollectionCoordinatorDocument"));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Namespace mismatch while running untrack, running coordinator nss: "
                          << _doc.getId().getNss().toStringForErrorMsg()
                          << ", issued nss: " << otherDoc.getId().getNss().toStringForErrorMsg(),
            _doc.getId().getNss() == otherDoc.getId().getNss());
}

void UntrackUnsplittableCollectionCoordinator::_checkPreconditions() {
    auto opCtxHolder = makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    _completeOnError = true;

    const auto chunkManager = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));

    if (!chunkManager.hasRoutingTable()) {
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "The collection " << nss().toStringForErrorMsg()
                                << " is not tracked");
    }
    // Skip non splittable collections.
    if (!chunkManager.isUnsplittable()) {
        uasserted(ErrorCodes::InvalidNamespace,
                  str::stream() << "Collection " << nss().toStringForErrorMsg()
                                << " must not be sharded");
    }
    tassert(8631100,
            "There must be only one chunk for unsplittable collections",
            chunkManager.numChunks() == 1);
    std::set<ShardId> shardIds;
    chunkManager.getAllShardIds(&shardIds);
    if (*shardIds.begin() != ShardingState::get(opCtx)->shardId()) {
        uasserted(ErrorCodes::OperationFailed,
                  "In order to untrack the collection it must be located in the primary db");
    }
    _completeOnError = false;
    ShardingLogging::get(opCtx)->logChange(opCtx, "untrackCollection.start", nss());
}

void UntrackUnsplittableCollectionCoordinator::_enterCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    auto service = ShardingRecoveryService::get(opCtx);
    service->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        nss(),
        _critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
    service->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        nss(),
        _critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    // Set the collection object in the document for the next phase.
    _doc.setOptCollType(sharding_ddl_util::getCollectionFromConfigServer(opCtx, nss()));
}

void UntrackUnsplittableCollectionCoordinator::_commitUntrackCollection(
    OperationContext* opCtx, std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    tassert(8631102, "There must be a collection stored in the document", _doc.getOptCollType());

    if (!_firstExecution) {
        _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, getNewSession(opCtx), **executor);
    }

    {
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
            opCtx,
            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
            Grid::get(opCtx)->catalogClient(),
            _doc.getOptCollType().get(),
            defaultMajorityWriteConcernDoNotUse(),
            session,
            **executor);
    }

    // Checkpoint the configTime to ensure that, in the case of a stepdown, the new primary will
    // start-up from a configTime that is inclusive of the metadata deletions that were committed
    // during the critical section.
    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

    // We need to send the drop to all the shards because movePrimary leaves garbage behind for
    // unsplittable collections.
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());
    {
        const auto session = getNewSession(opCtx);
        const auto uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
            opCtx,
            nss(),
            participants,
            **executor,
            session,
            true /* fromMigrate */,
            false /* dropSystemCollections */,
            uuid,
            true /*requireCollectionEmpty*/);
    }
}

void UntrackUnsplittableCollectionCoordinator::_exitCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    // Force a refresh of the filtering metadata to clean up the data structure held by the
    // CollectionShardingRuntime (Note also that this code is indirectly used to notify to secondary
    // nodes to clear their filtering information).
    FilteringMetadataCache::get(opCtx)->forceCollectionPlacementRefresh(opCtx, nss());
    FilteringMetadataCache::get(opCtx)->waitForCollectionFlush(opCtx, nss());

    // Ensures the refresh of the catalog cache will be waited majority at the end of the
    // command
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

    auto service = ShardingRecoveryService::get(opCtx);
    service->releaseRecoverableCriticalSection(
        opCtx,
        nss(),
        _critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        ShardingRecoveryService::FilteringMetadataClearer());

    ShardingLogging::get(opCtx)->logChange(opCtx, "untrackCollection.end", nss());
}

ExecutorFuture<void> UntrackUnsplittableCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kEnterCriticalSection) {
                _checkPreconditions();
            }
        })
        .then(
            _buildPhaseHandler(Phase::kEnterCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _enterCriticalSection(opCtx, executor, token); }))
        .then(_buildPhaseHandler(Phase::kCommit,
                                 [this, executor = executor, token, anchor = shared_from_this()](
                                     auto* opCtx) { _commitUntrackCollection(opCtx, executor); }))
        .then(
            _buildPhaseHandler(Phase::kReleaseCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _exitCriticalSection(opCtx, executor, token); }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }
            return status;
        });
}
}  // namespace mongo
