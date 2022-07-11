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


#include "mongo/db/s/migration_source_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/auto_split_vector.h"
#include "mongo/db/s/commit_chunk_migration_gen.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/type_shard_collection.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/duration.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration


namespace mongo {
namespace {

const auto msmForCsr = CollectionShardingRuntime::declareDecoration<MigrationSourceManager*>();

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);
const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutMigration);

std::string kEmptyErrMsgForMoveTimingHelper;

/*
 * Taking into account the provided max chunk size, returns:
 * - A `max` bound to perform split+move in case the chunk owning `min` is splittable.
 * - The `max` bound of the chunk owning `min in case it can't be split (too small or jumbo).
 */
BSONObj computeMaxBound(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& min,
                        const Chunk& owningChunk,
                        const ShardKeyPattern& skPattern,
                        const long long maxChunkSizeBytes) {
    // TODO SERVER-64926 do not assume min always present
    auto [splitKeys, _] = autoSplitVector(
        opCtx, nss, skPattern.toBSON(), min, owningChunk.getMax(), maxChunkSizeBytes, 1);
    if (splitKeys.size()) {
        return std::move(splitKeys.front());
    }

    return owningChunk.getMax();
}

MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep1);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep2);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep3);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep4);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep5);
MONGO_FAIL_POINT_DEFINE(moveChunkHangAtStep6);

MONGO_FAIL_POINT_DEFINE(failMigrationCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeLeavingCriticalSection);
MONGO_FAIL_POINT_DEFINE(migrationCommitNetworkError);
MONGO_FAIL_POINT_DEFINE(hangBeforePostMigrationCommitRefresh);

}  // namespace

MigrationSourceManager* MigrationSourceManager::get(CollectionShardingRuntime* csr,
                                                    CollectionShardingRuntime::CSRLock& csrLock) {
    return msmForCsr(csr);
}

std::shared_ptr<MigrationChunkClonerSource> MigrationSourceManager::getCurrentCloner(
    CollectionShardingRuntime* csr, CollectionShardingRuntime::CSRLock& csrLock) {
    auto msm = get(csr, csrLock);
    if (!msm)
        return nullptr;
    return msm->_cloneDriver;
}

MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               ShardsvrMoveRange&& request,
                                               WriteConcernOptions&& writeConcern,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
    : _opCtx(opCtx),
      _args(request),
      _writeConcern(writeConcern),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _stats(ShardingStatistics::get(_opCtx)),
      _critSecReason(BSON("command"
                          << "moveChunk"
                          << "fromShard" << _args.getFromShard() << "toShard"
                          << _args.getToShard())),
      _moveTimingHelper(_opCtx,
                        "from",
                        _args.getCommandParameter().ns(),
                        _args.getMin(),
                        _args.getMax(),
                        6,  // Total number of steps
                        &kEmptyErrMsgForMoveTimingHelper,
                        _args.getToShard(),
                        _args.getFromShard()) {
    invariant(!_opCtx->lockState()->isLocked());

    LOGV2(22016,
          "Starting chunk migration donation {requestParameters} with expected collection epoch "
          "{collectionEpoch}",
          "Starting chunk migration donation",
          "requestParameters"_attr = redact(_args.toBSON({})),
          "collectionEpoch"_attr = _args.getEpoch());

    _moveTimingHelper.done(1);
    moveChunkHangAtStep1.pauseWhileSet();

    // Make sure the latest shard version is recovered as of the time of the invocation of the
    // command.
    onShardVersionMismatch(_opCtx, nss(), boost::none);

    const auto shardId = ShardingState::get(opCtx)->shardId();

    // Complete any unfinished migration pending recovery
    {
        migrationutil::drainMigrationsPendingRecovery(opCtx);

        // Since the moveChunk command is holding the ActiveMigrationRegistry and we just drained
        // all migrations pending recovery, now there cannot be any document in
        // config.migrationCoordinators.
        PersistentTaskStore<MigrationCoordinatorDocument> store(
            NamespaceString::kMigrationCoordinatorsNamespace);
        invariant(store.count(opCtx) == 0);
    }

    // Snapshot the committed metadata from the time the migration starts
    const auto [collectionMetadata, collectionUUID] = [&] {
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IS);
        uassert(ErrorCodes::InvalidOptions,
                "cannot move chunks for a collection that doesn't exist",
                autoColl.getCollection());

        UUID collectionUUID = autoColl.getCollection()->uuid();

        auto* const csr = CollectionShardingRuntime::get(_opCtx, nss());
        const auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

        auto optMetadata = csr->getCurrentMetadataIfKnown();
        uassert(StaleConfigInfo(nss(),
                                ChunkVersion::IGNORED() /* receivedVersion */,
                                boost::none /* wantedVersion */,
                                shardId,
                                boost::none),
                "The collection's sharding state was cleared by a concurrent operation",
                optMetadata);

        auto& metadata = *optMetadata;
        uassert(StaleConfigInfo(nss(),
                                ChunkVersion::IGNORED() /* receivedVersion */,
                                ChunkVersion::UNSHARDED() /* wantedVersion */,
                                shardId,
                                boost::none),
                "Cannot move chunks for an unsharded collection",
                metadata.isSharded());

        // Atomically (still under the CSR lock held above) check whether migrations are allowed and
        // register the MigrationSourceManager on the CSR. This ensures that interruption due to the
        // change of allowMigrations to false will properly serialise and not allow any new MSMs to
        // be running after the change.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Collection is undergoing changes so moveChunk is not allowed.",
                metadata.allowMigrations());

        _scopedRegisterer.emplace(this, csr, csrLock);

        return std::make_tuple(std::move(metadata), std::move(collectionUUID));
    }();

    const auto collectionVersion = collectionMetadata.getCollVersion();
    const auto shardVersion = collectionMetadata.getShardVersion();

    uassert(StaleConfigInfo(nss(),
                            ChunkVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId,
                            boost::none),
            str::stream() << "cannot move chunk " << _args.toBSON({})
                          << " because collection may have been dropped. "
                          << "current epoch: " << collectionVersion.epoch()
                          << ", cmd epoch: " << _args.getEpoch(),
            _args.getEpoch() == collectionVersion.epoch());

    uassert(StaleConfigInfo(nss(),
                            ChunkVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId,
                            boost::none),
            str::stream() << "cannot move chunk " << _args.toBSON({})
                          << " because the shard doesn't contain any chunks",
            shardVersion.majorVersion() > 0);

    // Compute the max bound in case only `min` is set (moveRange)
    if (!_args.getMax().is_initialized()) {
        // TODO SERVER-64926 do not assume min always present
        const auto& min = *_args.getMin();

        const auto cm = collectionMetadata.getChunkManager();
        const auto owningChunk = cm->findIntersectingChunkWithSimpleCollation(min);
        const auto max = computeMaxBound(_opCtx,
                                         nss(),
                                         min,
                                         owningChunk,
                                         cm->getShardKeyPattern(),
                                         _args.getMaxChunkSizeBytes());
        _args.getMoveRangeRequestBase().setMax(max);
        _moveTimingHelper.setMax(max);
    }

    const auto& keyPattern = collectionMetadata.getKeyPattern();
    const bool validBounds = [&]() {
        // Return true if provided bounds are respecting the shard key format, false otherwise
        const auto nFields = keyPattern.nFields();
        if (nFields != (*_args.getMin()).nFields() || nFields != (*_args.getMax()).nFields()) {
            return false;
        }

        BSONObjIterator keyPatternIt(keyPattern), minIt(*_args.getMin()), maxIt(*_args.getMax());

        while (keyPatternIt.more()) {
            const auto keyPatternField = keyPatternIt.next().fieldNameStringData();
            const auto minField = minIt.next().fieldNameStringData();
            const auto maxField = maxIt.next().fieldNameStringData();

            if (keyPatternField != minField || keyPatternField != maxField) {
                return false;
            }
        }

        return true;
    }();
    uassert(StaleConfigInfo(nss(),
                            ChunkVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId,
                            boost::none),
            str::stream() << "Range bounds do not match the shard key pattern. KeyPattern:  "
                          << keyPattern.toString() << " - Bounds: "
                          << ChunkRange(*_args.getMin(), *_args.getMax()).toString() << ".",
            validBounds);

    ChunkType existingChunk;
    uassert(StaleConfigInfo(nss(),
                            ChunkVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId,
                            boost::none),
            str::stream() << "Range with bounds "
                          << ChunkRange(*_args.getMin(), *_args.getMax()).toString()
                          << " is not owned by this shard.",
            collectionMetadata.getNextChunk(*_args.getMin(), &existingChunk));

    uassert(StaleConfigInfo(nss(),
                            ChunkVersion::IGNORED() /* receivedVersion */,
                            shardVersion /* wantedVersion */,
                            shardId,
                            boost::none),
            str::stream() << "Unable to move range with bounds "
                          << ChunkRange(*_args.getMin(), *_args.getMax()).toString()
                          << " . The closest owned chunk is "
                          << ChunkRange(existingChunk.getMin(), existingChunk.getMax()).toString(),
            existingChunk.getRange().covers(ChunkRange(*_args.getMin(), *_args.getMax())));

    _collectionEpoch = collectionVersion.epoch();
    _collectionUUID = collectionUUID;

    _chunkVersion = collectionMetadata.getChunkManager()
                        ->findIntersectingChunkWithSimpleCollation(*_args.getMin())
                        .getLastmod();

    _moveTimingHelper.done(2);
    moveChunkHangAtStep2.pauseWhileSet();
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
    _stats.totalDonorMoveChunkTimeMillis.addAndFetch(_entireOpTimer.millis());

    _completion.emplaceValue();
}

void MigrationSourceManager::startClone() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.countDonorMoveChunkStarted.addAndFetch(1);

    uassertStatusOK(ShardingLogging::get(_opCtx)->logChangeChecked(
        _opCtx,
        "moveChunk.start",
        nss().ns(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard()),
        ShardingCatalogClient::kMajorityWriteConcern));

    _cloneAndCommitTimer.reset();

    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    auto replEnabled = replCoord->isReplEnabled();

    {
        const auto metadata = _getCurrentMetadataAndCheckEpoch();

        AutoGetCollection autoColl(_opCtx,
                                   nss(),
                                   replEnabled ? MODE_IX : MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                       Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));

        auto* const csr = CollectionShardingRuntime::get(_opCtx, nss());
        const auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

        // Having the metadata manager registered on the collection sharding state is what indicates
        // that a chunk on that collection is being migrated to the OpObservers. With an active
        // migration, write operations require the cloner to be present in order to track changes to
        // the chunk which needs to be transmitted to the recipient.
        _cloneDriver = std::make_shared<MigrationChunkClonerSourceLegacy>(
            _args, _writeConcern, metadata.getKeyPattern(), _donorConnStr, _recipientHost);

        _coordinator.emplace(_cloneDriver->getSessionId(),
                             _args.getFromShard(),
                             _args.getToShard(),
                             nss(),
                             *_collectionUUID,
                             ChunkRange(*_args.getMin(), *_args.getMax()),
                             *_chunkVersion,
                             _args.getWaitForDelete());

        _state = kCloning;
    }

    if (replEnabled) {
        auto const readConcernArgs = repl::ReadConcernArgs(
            replCoord->getMyLastAppliedOpTime(), repl::ReadConcernLevel::kLocalReadConcern);
        uassertStatusOK(waitForReadConcern(_opCtx, readConcernArgs, StringData(), false));

        setPrepareConflictBehaviorForReadConcern(
            _opCtx, readConcernArgs, PrepareConflictBehavior::kEnforce);
    }

    _coordinator->startMigration(_opCtx);

    uassertStatusOK(_cloneDriver->startClone(_opCtx,
                                             _coordinator->getMigrationId(),
                                             _coordinator->getLsid(),
                                             _coordinator->getTxnNumber()));

    _moveTimingHelper.done(3);
    moveChunkHangAtStep3.pauseWhileSet();
    scopedGuard.dismiss();
}

void MigrationSourceManager::awaitToCatchUp() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloning);
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    // Block until the cloner deems it appropriate to enter the critical section.
    uassertStatusOK(_cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        _opCtx, kMaxWaitToEnterCriticalSectionTimeout));

    _state = kCloneCaughtUp;
    _moveTimingHelper.done(4);
    moveChunkHangAtStep4.pauseWhileSet(_opCtx);
    scopedGuard.dismiss();
}

void MigrationSourceManager::enterCriticalSection() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    ScopeGuard scopedGuard([&] { _cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    const auto& metadata = _getCurrentMetadataAndCheckEpoch();

    // Check that there are no chunks on the recepient shard. Write an oplog event for change
    // streams if this is the first migration to the recipient.
    if (!metadata.getChunkManager()->getVersion(_args.getToShard()).isSet()) {
        migrationutil::notifyChangeStreamsOnRecipientFirstChunk(
            _opCtx, nss(), _args.getFromShard(), _args.getToShard(), _collectionUUID);
    }

    // Mark the shard as running critical operation, which requires recovery on crash.
    //
    // NOTE: The 'migrateChunkToNewShard' oplog message written by the above call to
    // 'notifyChangeStreamsOnRecipientFirstChunk' depends on this majority write to carry its
    // local write to majority committed.
    uassertStatusOK(ShardingStateRecovery::startMetadataOp(_opCtx));

    LOGV2_DEBUG_OPTIONS(4817402,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Starting critical section",
                        "migrationId"_attr = _coordinator->getMigrationId());

    _critSec.emplace(_opCtx, nss(), _critSecReason);

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This is will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the migration config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus = shardmetadatautil::updateShardCollectionsEntry(
        _opCtx,
        BSON(ShardCollectionType::kNssFieldName << nss().ns()),
        BSON("$inc" << BSON(ShardCollectionType::kEnterCriticalSectionCounterFieldName << 1)),
        false /*upsert*/);
    if (!signalStatus.isOK()) {
        uasserted(
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString());
    }

    LOGV2(22017,
          "Migration successfully entered critical section",
          "migrationId"_attr = _coordinator->getMigrationId());

    scopedGuard.dismiss();
}

void MigrationSourceManager::commitChunkOnRecipient() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    ScopeGuard scopedGuard([&] {
        _cleanupOnError();
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx,
                                                                   _args.getCommandParameter());
    });

    // Tell the recipient shard to fetch the latest changes.
    auto commitCloneStatus = _cloneDriver->commitClone(_opCtx);

    if (MONGO_unlikely(failMigrationCommit.shouldFail()) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    uassertStatusOKWithContext(commitCloneStatus, "commit clone failed");
    _recipientCloneCounts = commitCloneStatus.getValue()["counts"].Obj().getOwned();

    _state = kCloneCompleted;
    _moveTimingHelper.done(5);
    moveChunkHangAtStep5.pauseWhileSet();
    scopedGuard.dismiss();
}

void MigrationSourceManager::commitChunkMetadataOnConfig() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloneCompleted);

    ScopeGuard scopedGuard([&] {
        _cleanupOnError();
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss());
    });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = _getCurrentMetadataAndCheckEpoch();

        auto migratedChunk = MigratedChunkType(*_chunkVersion, *_args.getMin(), *_args.getMax());

        const auto currentTime = VectorClock::get(_opCtx)->getTime();

        CommitChunkMigrationRequest request(nss(),
                                            _args.getFromShard(),
                                            _args.getToShard(),
                                            migratedChunk,
                                            metadata.getCollVersion(),
                                            currentTime.clusterTime().asTimestamp());

        request.serialize({}, &builder);
        builder.append(kWriteConcernField, kMajorityWriteConcern.toBSON());
    }

    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server
    _critSec->enterCommitPhase();

    _state = kCommittingOnConfig;

    Timer t;

    auto commitChunkMigrationResponse =
        Grid::get(_opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            _opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            builder.obj(),
            Shard::RetryPolicy::kIdempotent);

    if (MONGO_unlikely(migrationCommitNetworkError.shouldFail())) {
        commitChunkMigrationResponse = Status(
            ErrorCodes::InternalError, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    Status migrationCommitStatus =
        Shard::CommandResponse::getEffectiveStatus(commitChunkMigrationResponse);

    if (!migrationCommitStatus.isOK()) {
        {
            UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
            AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
            CollectionShardingRuntime::get(_opCtx, nss())->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(_opCtx, nss());
        uassertStatusOK(migrationCommitStatus);
    }

    // Asynchronously tell the recipient to release its critical section
    _coordinator->launchReleaseRecipientCriticalSection(_opCtx);

    hangBeforePostMigrationCommitRefresh.pauseWhileSet();

    try {
        LOGV2_DEBUG_OPTIONS(4817404,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Starting post-migration commit refresh on the shard",
                            "migrationId"_attr = _coordinator->getMigrationId());

        forceShardFilteringMetadataRefresh(_opCtx, nss());

        LOGV2_DEBUG_OPTIONS(4817405,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard",
                            "migrationId"_attr = _coordinator->getMigrationId());
    } catch (const DBException& ex) {
        LOGV2_DEBUG_OPTIONS(4817410,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished post-migration commit refresh on the shard with error",
                            "migrationId"_attr = _coordinator->getMigrationId(),
                            "error"_attr = redact(ex));
        {
            UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
            AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
            CollectionShardingRuntime::get(_opCtx, nss())->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        // Best-effort recover of the shard version.
        onShardVersionMismatchNoExcept(_opCtx, nss(), boost::none).ignore();
        throw;
    }

    // Migration succeeded

    const auto refreshedMetadata = _getCurrentMetadataAndCheckEpoch();
    // Check if there are no chunks left on donor shard. Write an oplog event for change streams if
    // the last chunk migrated off the donor.
    if (!refreshedMetadata.getChunkManager()->getVersion(_args.getFromShard()).isSet()) {
        migrationutil::notifyChangeStreamsOnDonorLastChunk(
            _opCtx, nss(), _args.getFromShard(), _collectionUUID);
    }


    LOGV2(22018,
          "Migration succeeded and updated collection version to {updatedCollectionVersion}",
          "Migration succeeded and updated collection version",
          "updatedCollectionVersion"_attr = refreshedMetadata.getCollVersion(),
          "migrationId"_attr = _coordinator->getMigrationId());

    // If the migration has succeeded, clear the BucketCatalog so that the buckets that got migrated
    // out are no longer updatable.
    if (nss().isTimeseriesBucketsCollection()) {
        auto& bucketCatalog = BucketCatalog::get(_opCtx);
        bucketCatalog.clear(nss().getTimeseriesViewNamespace());
    }

    _coordinator->setMigrationDecision(DecisionEnum::kCommitted);

    hangBeforeLeavingCriticalSection.pauseWhileSet();

    scopedGuard.dismiss();

    _stats.totalCriticalSectionCommitTimeMillis.addAndFetch(t.millis());

    LOGV2(6107801,
          "Exiting commit critical section",
          "migrationId"_attr = _coordinator->getMigrationId(),
          "durationMillis"_attr = t.millis());

    // Exit the critical section and ensure that all the necessary state is fully persisted before
    // scheduling orphan cleanup.
    _cleanup(true);

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.commit",
        nss().ns(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard() << "counts" << *_recipientCloneCounts),
        ShardingCatalogClient::kMajorityWriteConcern);

    const ChunkRange range(*_args.getMin(), *_args.getMax());

    std::string orphanedRangeCleanUpErrMsg = str::stream()
        << "Moved chunks successfully but failed to clean up " << nss() << " range "
        << redact(range.toString()) << " due to: ";

    if (_args.getWaitForDelete()) {
        LOGV2(22019,
              "Waiting for migration cleanup after chunk commit for the namespace {namespace} "
              "and range {range}",
              "Waiting for migration cleanup after chunk commit",
              "namespace"_attr = nss(),
              "range"_attr = redact(range.toString()),
              "migrationId"_attr = _coordinator->getMigrationId());

        Status deleteStatus = _cleanupCompleteFuture
            ? _cleanupCompleteFuture->getNoThrow(_opCtx)
            : Status(ErrorCodes::Error(5089002),
                     "Not honouring the 'waitForDelete' request because migration coordinator "
                     "cleanup didn't succeed");
        if (!deleteStatus.isOK()) {
            uasserted(ErrorCodes::OrphanedRangeCleanUpFailed,
                      orphanedRangeCleanUpErrMsg + redact(deleteStatus));
        }
    }

    _moveTimingHelper.done(6);
    moveChunkHangAtStep6.pauseWhileSet();
}

void MigrationSourceManager::_cleanupOnError() noexcept {
    if (_state == kDone) {
        return;
    }

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.error",
        _args.getCommandParameter().ns(),
        BSON("min" << *_args.getMin() << "max" << *_args.getMax() << "from" << _args.getFromShard()
                   << "to" << _args.getToShard()),
        ShardingCatalogClient::kMajorityWriteConcern);

    _cleanup(true);
}

SharedSemiFuture<void> MigrationSourceManager::abort() {
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    _opCtx->markKilled();
    _stats.countDonorMoveChunkAbortConflictingIndexOperation.addAndFetch(1);

    return _completion.getFuture();
}

CollectionMetadata MigrationSourceManager::_getCurrentMetadataAndCheckEpoch() {
    auto metadata = [&] {
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, _args.getCommandParameter(), MODE_IS);
        auto* const css = CollectionShardingRuntime::get(_opCtx, _args.getCommandParameter());

        const auto optMetadata = css->getCurrentMetadataIfKnown();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "The collection's sharding state was cleared by a concurrent operation",
                optMetadata);
        return *optMetadata;
    }();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "The collection's epoch has changed since the migration began. "
                             "Expected collection epoch: "
                          << _collectionEpoch->toString() << ", but found: "
                          << (metadata.isSharded() ? metadata.getCollVersion().epoch().toString()
                                                   : "unsharded collection"),
            metadata.isSharded() && metadata.getCollVersion().epoch() == *_collectionEpoch);

    return metadata;
}

void MigrationSourceManager::_cleanup(bool completeMigration) noexcept {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state and exit the migration critical section.
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
        auto* const csr = CollectionShardingRuntime::get(_opCtx, nss());
        const auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

        if (_state != kCreated) {
            invariant(_cloneDriver);
        }

        _critSec.reset();
        return std::move(_cloneDriver);
    }();

    if (_state == kCriticalSection || _state == kCloneCompleted || _state == kCommittingOnConfig) {
        LOGV2_DEBUG_OPTIONS(4817403,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Finished critical section",
                            "migrationId"_attr = _coordinator->getMigrationId());

        LOGV2(6107802,
              "Finished critical section",
              "migrationId"_attr = _coordinator->getMigrationId(),
              "durationMillis"_attr = _cloneAndCommitTimer.millis());
    }

    // The cleanup operations below are potentially blocking or acquire other locks, so perform them
    // outside of the collection X lock

    if (cloneDriver) {
        cloneDriver->cancelClone(_opCtx);
    }

    try {
        if (_state >= kCloning) {
            invariant(_coordinator);
            if (_state < kCommittingOnConfig) {
                _coordinator->setMigrationDecision(DecisionEnum::kAborted);
            }

            auto newClient = _opCtx->getServiceContext()->makeClient("MigrationCoordinator");
            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillableByStepdown(lk);
            }
            AlternativeClientRegion acr(newClient);
            auto newOpCtxPtr = cc().makeOperationContext();
            auto newOpCtx = newOpCtxPtr.get();

            if (_state >= kCriticalSection && _state <= kCommittingOnConfig) {
                _stats.totalCriticalSectionTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());

                // NOTE: The order of the operations below is important and the comments explain the
                // reasoning behind it.
                //
                // Wait for the updates to the cache of the routing table to be fully written to
                // disk before clearing the 'minOpTime recovery' document. This way, we ensure that
                // all nodes from a shard, which donated a chunk will always be at the shard version
                // of the last migration it performed.
                //
                // If the metadata is not persisted before clearing the 'inMigration' flag below, it
                // is possible that the persisted metadata is rolled back after step down, but the
                // write which cleared the 'inMigration' flag is not, a secondary node will report
                // itself at an older shard version.
                CatalogCacheLoader::get(newOpCtx).waitForCollectionFlush(newOpCtx, nss());

                // Clear the 'minOpTime recovery' document so that the next time a node from this
                // shard becomes a primary, it won't have to recover the config server optime.
                ShardingStateRecovery::endMetadataOp(newOpCtx);
            }
            if (completeMigration) {
                // This can be called on an exception path after the OperationContext has been
                // interrupted, so use a new OperationContext. Note, it's valid to call
                // getServiceContext on an interrupted OperationContext.
                _cleanupCompleteFuture = _coordinator->completeMigration(newOpCtx);
            }
        }

        _state = kDone;
    } catch (const DBException& ex) {
        LOGV2_WARNING(5089001,
                      "Failed to complete the migration {migrationId} with "
                      "{chunkMigrationRequestParameters} due to: {error}",
                      "Failed to complete the migration",
                      "chunkMigrationRequestParameters"_attr = redact(_args.toBSON({})),
                      "error"_attr = redact(ex),
                      "migrationId"_attr = _coordinator->getMigrationId());
        // Something went really wrong when completing the migration just unset the metadata and let
        // the next op to recover.
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, nss(), MODE_IX);
        CollectionShardingRuntime::get(_opCtx, nss())->clearFilteringMetadata(_opCtx);
    }
}

BSONObj MigrationSourceManager::getMigrationStatusReport() const {
    return migrationutil::makeMigrationStatusDocument(
        _args.getCommandParameter(),
        _args.getFromShard(),
        _args.getToShard(),
        true,
        // TODO SERVER-64926 do not assume min always present
        *_args.getMin(),
        _args.getMax().value_or(BSONObj()));
}

MigrationSourceManager::ScopedRegisterer::ScopedRegisterer(
    MigrationSourceManager* msm,
    CollectionShardingRuntime* csr,
    const CollectionShardingRuntime::CSRLock& csrLock)
    : _msm(msm) {
    invariant(nullptr == std::exchange(msmForCsr(csr), msm));
}

MigrationSourceManager::ScopedRegisterer::~ScopedRegisterer() {
    UninterruptibleLockGuard noInterrupt(_msm->_opCtx->lockState());
    AutoGetCollection autoColl(_msm->_opCtx, _msm->_args.getCommandParameter(), MODE_IX);
    auto csr = CollectionShardingRuntime::get(_msm->_opCtx, _msm->_args.getCommandParameter());
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_msm->_opCtx, csr);
    invariant(_msm == std::exchange(msmForCsr(csr), nullptr));
}

}  // namespace mongo
