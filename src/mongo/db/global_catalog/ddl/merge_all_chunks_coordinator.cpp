/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/merge_all_chunks_coordinator.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/merge_chunk_request_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
static inline IDLParserContext kIdlParserCtx{"MergeAllChunksOnShardCoordinator"};

// Reason document attached to the recoverable critical section that brackets the authoritative
// commit. Must be stable across phases so that recovery releases the same section it acquired.
BSONObj makeCriticalSectionReason(const NamespaceString& nss) {
    return BSON(
        "command" << "mergeAllChunksOnShard"
                  << "ns"
                  << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
}

void checkPreconditions(OperationContext* opCtx, const NamespaceString& nss) {
    const bool allowChunkOps =
        CollectionShardingRuntime::acquireShared(opCtx, nss)->allowChunkOperations();

    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    const CollectionMetadata metadata(std::move(cm), ShardingState::get(opCtx)->shardHandle());

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " is not sharded",
            metadata.isSharded());

    uassert(
        ErrorCodes::ConflictingOperationInProgress,
        str::stream() << "Can't execute mergeAllChunksOnShard because chunk operations for this "
                         "collection are disallowed. 'allowChunkOperations' flag is "
                      << allowChunkOps << "; 'allowMigrations' flag is "
                      << metadata.allowMigrations(),
        allowChunkOps && metadata.allowMigrations());
}

void acquireCriticalSection(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const BSONObj& critSecReason) {
    auto svc = ShardingRecoveryService::get(opCtx);
    svc->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        nss,
        critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        false /* clearShardCatalogCache */);
    svc->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx, nss, critSecReason, ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

void releaseCriticalSection(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const BSONObj& critSecReason) {
    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        nss,
        critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        ShardingRecoveryService::NoCustomAction());
}

ConfigsvrMergeAllPrecomputedChunksOnShardResponse commitToGlobalCatalog(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shard,
    const mongo::BSONArray& newChunksBsonArray,
    OperationSessionInfo session) {

    std::vector<BSONObj> newChunks;
    for (const auto& elem : newChunksBsonArray) {
        newChunks.emplace_back(elem.Obj().getOwned());
    }

    ConfigSvrCommitMergeAllPrecomputedChunksOnShard configRequest(nss);
    configRequest.setDbName(DatabaseName::kAdmin);
    configRequest.setShard(shard);
    configRequest.setNewChunks(std::move(newChunks));
    generic_argument_util::setMajorityWriteConcern(configRequest);
    generic_argument_util::setOperationSessionInfo(configRequest, session);

    auto cmdResponse =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            configRequest.toBSON(),
            Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    return ConfigsvrMergeAllPrecomputedChunksOnShardResponse::parse(cmdResponse.response,
                                                                    kIdlParserCtx);
}

MergeAllChunksOnShardResponse commitToGlobalCatalogFallback(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const ShardId& shard,
                                                            std::int32_t maxNumberOfChunksToMerge,
                                                            std::int32_t maxTimeProcessingChunksMS,
                                                            OperationSessionInfo session) {
    ConfigSvrCommitMergeAllChunksOnShard configRequest(nss);
    configRequest.setDbName(DatabaseName::kAdmin);
    configRequest.setShard(shard);
    configRequest.setMaxNumberOfChunksToMerge(maxNumberOfChunksToMerge);
    configRequest.setMaxTimeProcessingChunksMS(maxTimeProcessingChunksMS);
    generic_argument_util::setMajorityWriteConcern(configRequest);
    generic_argument_util::setOperationSessionInfo(configRequest, session);

    auto cmdResponse =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            configRequest.toBSON(),
            Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
    return MergeAllChunksOnShardResponse::parse(cmdResponse.response, kIdlParserCtx);
}

boost::optional<CollectionType> getCollection(OperationContext* opCtx, const NamespaceString& nss) {
    PersistentTaskStore<CollectionType> collStore{
        NamespaceString::kConfigShardCatalogCollectionsNamespace};
    boost::optional<CollectionType> coll;
    collStore.forEach(opCtx,
                      BSON(CollectionType::kNssFieldName << nss.toStringForResourceId()),
                      [&](const CollectionType& parsedColl) {
                          coll = parsedColl;
                          return false;
                      });
    return coll;
}

// ConflictingOperationInProgress is raised by the config server before it modifies the global
// catalog (re-checked under the chunk-op lock). Receiving it during the commit phase proves the
// commit did not take effect, so the coordinator may safely abort and release the critical section.
bool isSafeToAbortOnGlobalCatalogCommitError(const Status& status) {
    return status == ErrorCodes::ConflictingOperationInProgress;
}

// NamespaceNotSharded and StaleEpoch indicate that the collection was dropped or its UUID changed
// while this coordinator was running. This should be impossible: all DDL operations that modify
// a collection's sharding state or UUID call setAllowChunkOperations(false) and drain in-flight
// chunk coordinators before making any change, so this coordinator cannot be running at the same
// time. If either error is seen at kGlobalCatalogCommit the DDL serialization invariant has been
// violated.
bool isNotExpectedOnGlobalCatalogCommitError(const Status& status) {
    return status == ErrorCodes::NamespaceNotSharded || status == ErrorCodes::StaleEpoch;
}

}  // namespace

MergeAllChunksCoordinator::MergeAllChunksCoordinator(ShardingCoordinatorService* service,
                                                     const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "MergeAllChunksCoordinator", initialStateDoc),
      _request(_doc.getShardsvrMergeAllChunksOnShardRequest()),
      _critSecReason(makeCriticalSectionReason(nss())) {}

void MergeAllChunksCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = MergeAllChunksCoordinatorDocument::parse(
        doc, IDLParserContext("MergeAllChunksCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrMergeAllChunksOnShardRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another merge all chunks operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void MergeAllChunksCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool MergeAllChunksCoordinator::isInCriticalSection(Phase phase) const {
    return _doc.getPhase() >= Phase::kAcquireCriticalSection;
}

bool MergeAllChunksCoordinator::_mustAlwaysMakeProgress() {
    // Once the critical section has been (or is about to be) acquired, the coordinator must
    // either finish committing or release the critical section via _cleanupOnAbort. Force the retry
    // loop to keep running so transient failures in either path cannot leave the critical section
    // held.
    return _doc.getPhase() >= Phase::kAcquireCriticalSection;
}

bool MergeAllChunksCoordinator::_allowedToInvalidateOSI() const noexcept {
    // TODO (SERVER-127444): remove !_doc.getNewChunks().has_value() from the condition.
    return _cleaningUp || !_doc.getNewChunks().has_value() ||
        _doc.getPhase() != Phase::kGlobalCatalogCommit;
}

void MergeAllChunksCoordinator::_onCleanup(OperationContext* opCtx) {
    _cleaningUp = true;
    ChunkOperationShardingCoordinator<MergeAllChunksCoordinatorDocument>::_onCleanup(opCtx);
}

ExecutorFuture<void> MergeAllChunksCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = makeOperationContext();
        auto* opCtx = opCtxHolder.get();
        if (_doc.getPhase() >= Phase::kAcquireCriticalSection) {
            releaseCriticalSection(opCtx, nss(), _critSecReason);
        }
    });
}

ExecutorFuture<void> MergeAllChunksCoordinator::_acquireLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
               auto* newOpCtx = opCtxHolder.get();

               // Span the whole shard key space so the registration acts as a
               // namespace-wide guard (see class comment for details).
               const auto chunkRange = ChunkRange(kMinBSONKey, kMaxBSONKey);
               _scopedSplitMergeChunk.emplace(uassertStatusOK(
                   ActiveMigrationsRegistry::get(newOpCtx).registerSplitOrMergeChunk(
                       newOpCtx, nss(), chunkRange, makeRegistryRecoveryBypass())));
           })
        .until([this, anchor = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(12578601,
                              "ActiveMigrationsRegistry lock acquisition attempt failed",
                              logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                       "error"_attr = redact(status)});
            }
            // If this coordinator is recovering after a failover, acquiring the
            // ActiveMigrationsRegistry must succeed regardless of the current state, since the
            // operation may have been left half-complete.
            return !_recoveredFromDisk || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

void MergeAllChunksCoordinator::_releaseLocks(OperationContext* opCtx) {
    _scopedSplitMergeChunk.reset();
}

ExecutorFuture<void> MergeAllChunksCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    // Half the max BSON size to have space for the rest of the document.
    static constexpr auto kMaxChunkVectorSize = BSONObjMaxUserSize / 2;

    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, anchor = shared_from_this()](auto* opCtx) {
                tassert(12578602,
                        "Chunk operation coordinator must not run without authoritative metadata "
                        "access",
                        _doc.getAuthoritativeMetadataAccessLevel() !=
                            AuthoritativeMetadataAccessLevelEnum::kNone);

                LOGV2(12578603,
                      "Checking preconditions for merge all chunks on shard operation",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                checkPreconditions(opCtx, nss());
            }))
        .then(_buildPhaseHandler(
            Phase::kPrecomputeChunkList,
            [this, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12834400,
                      "Precompute list of chunks to merge",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                const auto coll = getCollection(opCtx, nss());
                if (!coll) {
                    LOGV2(12744401,
                          "Collection not found in local shard catalog, nothing to merge",
                          logAttrs(nss()),
                          "shard"_attr = _request.getShard());
                    _doc.setNumMergedChunks(0);
                    _doc.setShardVersion(ChunkVersion{});
                    _enterPhase(Phase::kReleaseCriticalSection);
                    return;
                }

                const auto [newChunks, _1, _2, numMergedChunks] =
                    sharding_ddl_util::computeAllMergeableChunksOnShard(
                        opCtx,
                        nss(),
                        _request.getShard(),
                        BSONObj{},
                        Grid::get(opCtx)->shardRegistry()->getConfigShard(),
                        NamespaceString::kConfigShardCatalogChunksNamespace,
                        *coll,
                        boost::none /*originalVersion*/,
                        _request.getMaxNumberOfChunksToMerge(),
                        _request.getMaxTimeProcessingChunksMS());

                if (numMergedChunks == 0) {
                    LOGV2(12834408,
                          "There are no chunks to merge, skipping commit phase",
                          logAttrs(nss()),
                          "shard"_attr = _request.getShard());
                    // There are no chunks to merge, so we can short-circuit and avoid taking the
                    // critical section.
                    _doc.setNumMergedChunks(numMergedChunks);
                    // The chunkVersion is unused in this case, it's ok to return an empty version.
                    _doc.setShardVersion(ChunkVersion{});
                    _enterPhase(Phase::kReleaseCriticalSection);
                    return;
                }

                BSONArrayBuilder bab;
                for (const auto& chunk : newChunks) {
                    BSONObj chunkBson = chunk.toConfigBSON(true /*omitVersion*/);
                    if (bab.len() + chunkBson.objsize() > kMaxChunkVectorSize) {
                        // If we truncate the list, numMergedChunks is no longer correct. This is
                        // not a huge deal: that number never reaches the user, and internally is
                        // only ever used by the balancer by comparing it to 0, so the answer is
                        // "right" so long as it's greater than zero.
                        // TODO (SERVER-128771): make it so `computeAllMergeableChunksOnShard`
                        // returns a BSONArray directly and imposes this limit internally.
                        LOGV2_WARNING(12834409,
                                      "Truncating chunk list because the list is too large",
                                      "bsonArraySizeBytes"_attr = bab.len(),
                                      "newChunkSizeBytes"_attr = chunkBson.objsize(),
                                      "newChunksLength"_attr = newChunks.size(),
                                      "bsonArrayLength"_attr = bab.arrSize());
                        break;
                    }
                    bab.append(std::move(chunkBson));
                }

                auto newDoc = _doc;
                newDoc.setNewChunks(bab.arr());
                newDoc.setNumMergedChunks(numMergedChunks);
                _updateStateDocument(opCtx, std::move(newDoc));
            }))
        .then(_buildPhaseHandler(
            Phase::kAcquireCriticalSection,
            [this, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578604,
                      "Acquiring critical section for merge all chunks on shard operation",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                acquireCriticalSection(opCtx, nss(), _critSecReason);

                // Generate a new OSI to be used during next phase
                getNewSession(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kGlobalCatalogCommit,
            [this, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578605,
                      "Committing merge all chunks on shard operation to the global catalog",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                // TODO (SERVER-127444): remove the "else" branch and add tassert on
                // _doc.getNewChunks().has_value()
                if (_doc.getNewChunks().has_value()) {
                    // _configSvrCommitMergeAllPrecomputedChunksOnShard is idempotent throught
                    // retryable writes, so we need to use the same session (i.e. same
                    // lsid/txnNumber) on every retry. For this reason, we don't generate a new
                    // OSI but rather resuse the current one. This OSI was generated at the end of
                    // the previous phase.
                    const auto session = getCurrentSession(opCtx);
                    tassert(12834402, "There must be a persisted session", session.has_value());

                    auto response = commitToGlobalCatalog(
                        opCtx, nss(), _request.getShard(), *_doc.getNewChunks(), *session);

                    auto newDoc = _doc;
                    newDoc.setShardVersion(response.getShardVersion());
                    _updateStateDocument(opCtx, std::move(newDoc));
                } else {
                    // This is the legacy commit, which calls
                    // `_configSvrCommitMergeAllChunksOnShardCommand` instead of
                    // `_configSvrCommitMergeAllPrecomputedChunksOnShard`. This branch is reached
                    // only when the catalog is non-authoritative when this command runs.
                    auto response =
                        commitToGlobalCatalogFallback(opCtx,
                                                      nss(),
                                                      _request.getShard(),
                                                      _request.getMaxNumberOfChunksToMerge(),
                                                      _request.getMaxTimeProcessingChunksMS(),
                                                      getNewSession(opCtx));
                    auto newDoc = _doc;
                    newDoc.setShardVersion(response.getShardVersion());
                    newDoc.setNumMergedChunks(response.getNumMergedChunks());
                    _updateStateDocument(opCtx, std::move(newDoc));
                }

                // Checkpoint the configTime to ensure that, in the case of a stepdown, the new
                // primary will start-up from a configTime that is inclusive of the metadata update
                // that has been committed.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kShardCatalogCommit,
            [this, executor, token, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578607,
                      "Installing new chunk layout into the shard catalog after merge all chunks",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                // Install the new chunk layout into the local shard catalog.
                sharding_ddl_util::commitCreateCollectionMetadataToShardCatalog(
                    opCtx,
                    nss(),
                    {ShardingState::get(opCtx)->shardId()},
                    getNewSession(opCtx),
                    executor,
                    token);
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSection,
            [this, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578608,
                      "Releasing critical section for merge all chunks on shard "
                      "operation",
                      logAttrs(nss()),
                      "shard"_attr = _request.getShard());

                releaseCriticalSection(opCtx, nss(), _critSecReason);

                tassert(12834403,
                        fmt::format("Expected response fields to be set: {} {}",
                                    _doc.getShardVersion().has_value(),
                                    _doc.getNumMergedChunks().has_value()),
                        _doc.getShardVersion().has_value() &&
                            _doc.getNumMergedChunks().has_value());
                _response.emplace(*_doc.getShardVersion(), *_doc.getNumMergedChunks());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            // Retry in case of getting a retryable error.
            if (_isRetriableErrorForDDLCoordinator(status)) {
                return status;
            }

            const auto phase = _doc.getPhase();

            // Before the kGlobalCatalogCommit phase the merge has not been committed anywhere, so
            // a non-retryable error is safe to abort on. Persist an abort reason so the critical
            // section (if already held) is released via _cleanupOnAbort.
            if (phase < Phase::kGlobalCatalogCommit) {
                auto opCtxHolder = makeOperationContext();
                triggerCleanup(opCtxHolder.get(), status);
                MONGO_UNREACHABLE_TASSERT(12578610);
            }

            if (phase == Phase::kGlobalCatalogCommit) {
                // These errors indicate the DDL serialization invariant was violated: all DDL
                // operations drain in-flight chunk coordinators before modifying the collection,
                // so they cannot be observed here under correct operation.
                tassert(12925601,
                        str::stream() << "Unexpected error during mergeAllChunksOnShard global "
                                         "catalog commit: "
                                      << status.toString(),
                        !isNotExpectedOnGlobalCatalogCommitError(status));

                // Abort only for errors that are raised before the commit is applied. For any
                // other error keep retrying so the global and shard catalogs cannot diverge.
                if (isSafeToAbortOnGlobalCatalogCommitError(status)) {
                    auto opCtxHolder = makeOperationContext();
                    triggerCleanup(opCtxHolder.get(), status);
                    MONGO_UNREACHABLE_TASSERT(12925602);
                }
            }

            // Retry the operation when kGlobalCatalogCommit or a greater phase fails.
            return status;
        });
}

}  // namespace mongo
