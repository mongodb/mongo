// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/split_chunk_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/commit_split_chunk_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/ddl/split_chunk.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

// Reason document attached to the recoverable critical section that brackets the authoritative
// commit. Must be stable across phases so that recovery releases the same section it acquired.
BSONObj makeCriticalSectionReason(const NamespaceString& nss) {
    return BSON(
        "command" << "splitChunk"
                  << "ns"
                  << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
}

// Validates that the split request can proceed and reports whether it is an idempotent
// no-op.
// Returns true iff the request is an idempotent no-op; false otherwise.
bool checkPreconditions(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const ShardsvrSplitChunkRequest& request) {
    // Validate the request bounds and split points
    const ChunkRange chunkRange(request.getMin(), request.getMax());
    uassertStatusOK(ChunkRange::validate(chunkRange));
    uassert(
        ErrorCodes::InvalidOptions, "No split points provided", !request.getSplitKeys().empty());
    const std::vector<BSONObj> splitKeys(request.getSplitKeys().begin(),
                                         request.getSplitKeys().end());

    // Anticipate any BSONTooLarge error that might occur after committing on the config server.
    // Once the config server commit succeeds, the operation cannot be rolled back, so we must
    // avoid failing with a non-retryable error.
    //
    // Assume the worst case: each split point is stored three times in the coordinator document:
    // once in the list of split points, and once each as the min and max bounds of the updated
    // chunks.
    long long estimatedMetadataSize = chunkRange.getMin().objsize() + chunkRange.getMax().objsize();
    for (const auto& splitKey : splitKeys) {
        estimatedMetadataSize += 3 * splitKey.objsize();
    }
    estimatedMetadataSize = 2 * estimatedMetadataSize;

    uassert(ErrorCodes::BSONObjectTooLarge,
            str::stream() << "Refusing to split chunk " << chunkRange.toString()
                          << ": estimated resulting chunk metadata (~" << estimatedMetadataSize
                          << " bytes) would exceed the maximum BSON object size ("
                          << BSONObjMaxUserSize << " bytes)",
            estimatedMetadataSize < BSONObjMaxUserSize);

    // Read the CSR's allowChunkOperations flag up front.
    // We snapshot the CSR before fetching CollectionMetadata so the metadata checks run against
    // a stable view.
    // The actual validation happens only after the idempotency check below, so this early read is
    // intentional and separate from the later assertion.
    const bool allowChunkOps =
        CollectionShardingRuntime::acquireShared(opCtx, nss)->allowChunkOperations();

    // Obtain the cluster-wide chunk layout by forcing a refresh from the global
    // catalog.
    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    const CollectionMetadata metadata(std::move(cm), ShardingState::get(opCtx)->shardId());

    // Validate that the collection is sharded and that its identity (epoch / timestamp) still
    // matches the request.
    checkCollectionIdentity(opCtx, nss, request.getEpoch(), request.getTimestamp(), metadata);

    // Validate structural correctness: shard-key pattern of the bounds, that the requested range
    // exists as a single chunk on this shard, and that the split points are valid.
    checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
    checkChunkMatchesRange(opCtx, nss, metadata, chunkRange);
    validateSplitPoints(opCtx, nss, metadata, chunkRange, splitKeys);

    // Detect an idempotent no-op: if every requested split point already lies on a chunk boundary
    // on this shard, a previous successful commit has already created the target chunks. In that
    // case, return true.
    //
    // This check must happen before checking the allowChunkOperations flag, because the public
    // idempotency contract requires retries to succeed even if chunk operations were disallowed
    // after the original command completed successfully.
    bool isAlreadyCommitted = true;
    BSONObj cursor = chunkRange.getMin();
    for (const auto& splitKey : splitKeys) {
        ChunkType existingChunk;
        if (!metadata.getNextChunk(cursor, &existingChunk) ||
            existingChunk.getMin().woCompare(cursor) != 0 ||
            existingChunk.getMax().woCompare(splitKey) != 0) {
            isAlreadyCommitted = false;
            break;
        }
        cursor = splitKey;
    }
    if (isAlreadyCommitted) {
        return true;
    }

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Can't execute splitChunks because chunk operations for this "
                             "collection are disallowed. 'allowChunkOperations' flag is "
                          << allowChunkOps << "; 'allowMigrations' flag is "
                          << metadata.allowMigrations(),
            allowChunkOps && metadata.allowMigrations());

    return false;
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

// Commits the split to the global catalog by issuing the idempotent '_configsvrCommitSplitChunk'
// command and returns the resulting sub-chunks that the shard catalog must install.
// 'shardVersionPreSplit' is the shard's placement version captured before the split; the config
// server uses it to validate the request and to rebuild the same set of changed chunks on an
// idempotent retry.
std::vector<BSONObj> commitToGlobalCatalog(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const ShardId& shardId,
                                           const ChunkRange& chunkRange,
                                           const std::vector<BSONObj>& splitKeys,
                                           const ChunkVersion& shardVersionPreSplit,
                                           OperationSessionInfo session) {
    ConfigSvrCommitSplitChunkRequest request(nss);
    request.setDbName(DatabaseName::kAdmin);
    request.setShard(ShardRef(shardId));
    request.setRange(chunkRange);
    request.setSplitPoints(splitKeys);
    request.setShardVersionPreSplit(shardVersionPreSplit);
    generic_argument_util::setMajorityWriteConcern(request);
    generic_argument_util::setOperationSessionInfo(request, session);

    auto cmdResponse = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithIndefiniteRetries(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            request.toBSON(),
            Shard::RetryPolicy::kIdempotent));

    uassertStatusOKWithContext(cmdResponse.commandStatus, "Failed to commit chunk split");
    uassertStatusOKWithContext(cmdResponse.writeConcernStatus, "Failed to commit chunk split");

    auto response = ConfigSvrCommitSplitChunkResponse::parse(
        cmdResponse.response, IDLParserContext("ConfigSvrCommitSplitChunkResponse"));

    std::vector<BSONObj> changedChunks;
    changedChunks.reserve(response.getChangedChunks().size());
    for (const auto& chunk : response.getChangedChunks()) {
        changedChunks.push_back(chunk.getOwned());
    }
    return changedChunks;
}

// Errors that ShardingCatalogManager::commitChunkSplit raises strictly before it modifies the
// global catalog. Receiving one of these during the commit phase proves the commit did not take
// effect, so the coordinator may safely abort and release the critical section. Any other
// non-retryable error at commit time is ambiguous and must be retried.
bool isExpectedGlobalCatalogCommitError(const Status& status) {
    return status == ErrorCodes::ConflictingOperationInProgress;
}

}  // namespace

SplitChunkCoordinator::SplitChunkCoordinator(ShardingCoordinatorService* service,
                                             const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "SplitChunkCoordinator", initialStateDoc),
      _request(_doc.getShardsvrSplitChunkRequest()),
      _critSecReason(makeCriticalSectionReason(nss())) {}

void SplitChunkCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = SplitChunkCoordinatorDocument::parse(
        doc, IDLParserContext("SplitChunkCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrSplitChunkRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another split chunk operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void SplitChunkCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

bool SplitChunkCoordinator::isInCriticalSection(Phase phase) const {
    return _doc.getPhase() >= Phase::kAcquireCriticalSection;
}

bool SplitChunkCoordinator::_mustAlwaysMakeProgress() {
    // Once the critical section has been (or is about to be) acquired, the coordinator must
    // either finish committing or release the critical section via _cleanupOnAbort. Force the retry
    // loop to keep running so transient failures in either path cannot leave the critical section
    // held.
    return _doc.getPhase() >= Phase::kAcquireCriticalSection;
}

ExecutorFuture<void> SplitChunkCoordinator::_cleanupOnAbort(
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

ExecutorFuture<void> SplitChunkCoordinator::_acquireLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext(/*deprioritizable=*/true);
               auto* opCtx = opCtxHolder.get();

               auto chunkRange = ChunkRange(_request.getMin(), _request.getMax());
               _scopedSplitMergeChunk.emplace(
                   uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
                       opCtx, nss(), chunkRange, makeRegistryRecoveryBypass())));
           })
        .until([this, anchor = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(12117803,
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

void SplitChunkCoordinator::_releaseLocks(OperationContext* opCtx) {
    _scopedSplitMergeChunk.reset();
}

ExecutorFuture<void> SplitChunkCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    auto shouldExecuteIfNotCommitted = [this](OperationContext*) {
        return !_alreadyCommitted;
    };

    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, anchor = shared_from_this()](auto* opCtx) {
                tassert(12578501,
                        "Chunk operation coordinator must not run without authoritative metadata "
                        "access",
                        _doc.getAuthoritativeMetadataAccessLevel() !=
                            AuthoritativeMetadataAccessLevelEnum::kNone);

                LOGV2(12578502,
                      "Checking preconditions for split chunk operation",
                      logAttrs(nss()),
                      "range"_attr = ChunkRange(_request.getMin(), _request.getMax()).toString());

                _registerChunkOperationStarted(opCtx);

                _alreadyCommitted = checkPreconditions(opCtx, nss(), _request);

                // Capture and persist the shard's placement version before the first commit
                // attempt. The configsvr commit is idempotent on this version, so it must remain
                // stable across retries and failovers; otherwise a retry that ran after the commit
                // already applied would observe the post-split version and rebuild no chunks.
                if (!_alreadyCommitted && !_doc.getShardVersionPreSplit()) {
                    auto cm = uassertStatusOK(
                        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(
                            opCtx, nss()));
                    const CollectionMetadata metadata(std::move(cm),
                                                      ShardingState::get(opCtx)->shardId());

                    auto newDoc = _doc;
                    newDoc.setShardVersionPreSplit(metadata.getShardPlacementVersion());
                    _updateStateDocument(opCtx, std::move(newDoc));
                }
            }))
        .then(_buildPhaseHandler(Phase::kAcquireCriticalSection,
                                 shouldExecuteIfNotCommitted,
                                 [this, anchor = shared_from_this()](auto* opCtx) {
                                     auto chunkRange =
                                         ChunkRange(_request.getMin(), _request.getMax());

                                     LOGV2(12578503,
                                           "Acquiring critical section for split chunk operation",
                                           logAttrs(nss()),
                                           "range"_attr = chunkRange.toString());

                                     acquireCriticalSection(opCtx, nss(), _critSecReason);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kGlobalCatalogCommit,
            shouldExecuteIfNotCommitted,
            [this, anchor = shared_from_this()](auto* opCtx) {
                auto chunkRange = ChunkRange(_request.getMin(), _request.getMax());
                std::vector<BSONObj> splitKeys(_request.getSplitKeys().begin(),
                                               _request.getSplitKeys().end());

                LOGV2(12117800,
                      "Committing split chunk operation to the global catalog",
                      logAttrs(nss()),
                      "range"_attr = chunkRange.toString());


                // Set during kCheckPreconditions, before this phase can run. Assert explicitly
                // instead of dereferencing an empty optional if that ever changes.
                tassert(12933302,
                        "Expected persisted pre-split shard version before committing split to the "
                        "global catalog",
                        _doc.getShardVersionPreSplit());

                auto changedChunks = commitToGlobalCatalog(opCtx,
                                                           nss(),
                                                           ShardingState::get(opCtx)->shardId(),
                                                           chunkRange,
                                                           splitKeys,
                                                           *_doc.getShardVersionPreSplit(),
                                                           getNewSession(opCtx));

                // Persist the changed chunks so the shard catalog commit phase can install them
                // even after a failover, without contacting the config server again.
                auto newDoc = _doc;
                newDoc.setChangedChunks(std::move(changedChunks));
                _updateStateDocument(opCtx, std::move(newDoc));

                // Checkpoint the configTime to ensure that, in the case of a stepdown, the new
                // primary will start-up from a configTime that is inclusive of the metadata update
                // that has been committed.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kShardCatalogCommit,
            shouldExecuteIfNotCommitted,
            [this, executor, token, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578504,
                      "Installing new chunk layout into the shard catalog after split",
                      logAttrs(nss()),
                      "range"_attr = ChunkRange(_request.getMin(), _request.getMax()).toString());

                // A committed split always produces at least one changed chunk. Reaching this
                // phase without any means the global catalog commit result was not persisted,
                // which would silently install nothing into the shard catalog.
                tassert(12933301,
                        "Expected persisted changed chunks after committing split to the global "
                        "catalog",
                        _doc.getChangedChunks() && !_doc.getChangedChunks()->empty());

                // Forward the committed chunks to the shard catalog.
                // Note that all the updated chunks are relevant for both shards donor and recipient
                // to persist the PIT read history.
                sharding_ddl_util::commitChunkOperationsMetadataToShardCatalog(
                    opCtx,
                    nss(),
                    *_doc.getChangedChunks(),
                    {ShardingState::get(opCtx)->shardId()},
                    getNewSession(opCtx),
                    executor,
                    token);

                // A split of one chunk on N points yields N + 1 chunks.
                ShardingStatistics::get(opCtx)
                    .chunkOperationsStatistics.registerSplitChunkResultingChunks(
                        _request.getSplitKeys().size() + 1);
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSection,
            shouldExecuteIfNotCommitted,
            [this, anchor = shared_from_this()](auto* opCtx) {
                LOGV2(12578505,
                      "Releasing critical section for split chunk operation",
                      logAttrs(nss()),
                      "range"_attr = ChunkRange(_request.getMin(), _request.getMax()).toString());

                releaseCriticalSection(opCtx, nss(), _critSecReason);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            // Retry in case of getting a retryable error.
            if (_isRetriableErrorForDDLCoordinator(status)) {
                return status;
            }

            const auto phase = _doc.getPhase();

            // Before the kGlobalCatalogCommit phase the split has not been committed anywhere, so
            // a non-retryable error is safe to abort on. Persist an abort reason so the critical
            // section (if already held) is released via _cleanupOnAbort.
            if (phase < Phase::kGlobalCatalogCommit) {
                auto opCtxHolder = makeOperationContext();
                triggerCleanup(opCtxHolder.get(), status);
                MONGO_UNREACHABLE_TASSERT(12578506);
            }

            // Abort during the global catalog commit only for errors that we know are raised before
            // the commit is applies. For any other error keep retrying so the global and shard
            // catalogs cannot diverge.
            if (phase == Phase::kGlobalCatalogCommit &&
                isExpectedGlobalCatalogCommitError(status)) {
                auto opCtxHolder = makeOperationContext();
                triggerCleanup(opCtxHolder.get(), status);
                MONGO_UNREACHABLE_TASSERT(12578507);
            }

            // Retry the operation when either kGlobalCatalogCommit fails with an unexpected error,
            // or a phase greater than kGlobalCatalogCommit fails with a non-retryable error.
            return status;
        });
}

}  // namespace mongo
