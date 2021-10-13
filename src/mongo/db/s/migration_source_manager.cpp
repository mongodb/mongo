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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_source_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/replication_coordinator.h"
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
#include "mongo/s/request_types/commit_chunk_migration_request_type.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/duration.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

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

/**
 * Best-effort attempt to ensure the recipient shard has refreshed its routing table to
 * 'newCollVersion'. Fires and forgets an asychronous remote setShardVersion command.
 */
void refreshRecipientRoutingTable(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const HostAndPort& toShardHost,
                                  const ChunkVersion& newCollVersion) {
    SetShardVersionRequest ssv(nss, newCollVersion, false);

    const executor::RemoteCommandRequest request(
        toShardHost,
        NamespaceString::kAdminDb.toString(),
        ssv.toBSON(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toContainingBSON(),
        opCtx,
        executor::RemoteCommandRequest::kNoTimeout);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto noOp = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    executor->scheduleRemoteCommand(request, noOp).getStatus().ignore();
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(doNotRefreshRecipientAfterCommit);
MONGO_FAIL_POINT_DEFINE(failMigrationCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeLeavingCriticalSection);
MONGO_FAIL_POINT_DEFINE(migrationCommitNetworkError);
MONGO_FAIL_POINT_DEFINE(hangBeforePostMigrationCommitRefresh);

MigrationSourceManager* MigrationSourceManager::get(CollectionShardingRuntime* csr,
                                                    CollectionShardingRuntime::CSRLock& csrLock) {
    return msmForCsr(csr);
}

MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               MoveChunkRequest request,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
    : _opCtx(opCtx),
      _args(std::move(request)),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _stats(ShardingStatistics::get(_opCtx)),
      _critSecReason(BSON("command"
                          << "moveChunk"
                          << "fromShard" << _args.getFromShardId() << "toShard"
                          << _args.getToShardId())) {
    invariant(!_opCtx->lockState()->isLocked());

    LOGV2(22016,
          "Starting chunk migration donation {requestParameters} with expected collection epoch "
          "{collectionEpoch}",
          "Starting chunk migration donation",
          "requestParameters"_attr = redact(_args.toString()),
          "collectionEpoch"_attr = _args.getVersionEpoch());

    // Make sure the latest shard version is recovered as of the time of the invocation of the
    // command.
    onShardVersionMismatch(_opCtx, getNss(), boost::none);

    // Snapshot the committed metadata from the time the migration starts
    const auto [collectionMetadata, collectionUUID] = [&] {
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, getNss(), MODE_IS);
        uassert(ErrorCodes::InvalidOptions,
                "cannot move chunks for a collection that doesn't exist",
                autoColl.getCollection());

        UUID collectionUUID = autoColl.getCollection()->uuid();

        auto optMetadata =
            CollectionShardingRuntime::get(_opCtx, getNss())->getCurrentMetadataIfKnown();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "The collection's sharding state was cleared by a concurrent operation",
                optMetadata);

        auto& metadata = *optMetadata;
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                "Cannot move chunks for an unsharded collection",
                metadata.isSharded());
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Collection is undergoing changes so moveChunk is not allowed.",
                metadata.allowMigrations());

        return std::make_tuple(std::move(metadata), std::move(collectionUUID));
    }();

    const auto collectionVersion = collectionMetadata.getCollVersion();
    const auto shardVersion = collectionMetadata.getShardVersion();

    // If the shard major version is zero, this means we do not have any chunks locally to migrate
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because the shard doesn't contain any chunks",
            shardVersion.majorVersion() > 0);

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because collection may have been dropped. "
                          << "current epoch: " << collectionVersion.epoch()
                          << ", cmd epoch: " << _args.getVersionEpoch(),
            _args.getVersionEpoch() == collectionVersion.epoch());

    ChunkType chunkToMove;
    chunkToMove.setMin(_args.getMinKey());
    chunkToMove.setMax(_args.getMaxKey());

    uassertStatusOKWithContext(collectionMetadata.checkChunkIsValid(chunkToMove),
                               str::stream() << "Unable to move chunk with arguments '"
                                             << redact(_args.toString()));

    _collectionEpoch = collectionVersion.epoch();
    _collectionUUID = collectionUUID;

    _chunkVersion = collectionMetadata.getChunkManager()
                        ->findIntersectingChunkWithSimpleCollation(_args.getMinKey())
                        .getLastmod();
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
    _stats.totalDonorMoveChunkTimeMillis.addAndFetch(_entireOpTimer.millis());
}

NamespaceString MigrationSourceManager::getNss() const {
    return _args.getNss();
}

Status MigrationSourceManager::startClone() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    ScopeGuard scopedGuard([&] { cleanupOnError(); });
    _stats.countDonorMoveChunkStarted.addAndFetch(1);

    const Status logStatus = ShardingLogging::get(_opCtx)->logChangeChecked(
        _opCtx,
        "moveChunk.start",
        getNss().ns(),
        BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                   << _args.getFromShardId() << "to" << _args.getToShardId()),
        ShardingCatalogClient::kMajorityWriteConcern);
    if (logStatus != Status::OK()) {
        return logStatus;
    }

    _cloneAndCommitTimer.reset();

    auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
    auto replEnabled = replCoord->isReplEnabled();

    {
        const auto metadata = _getCurrentMetadataAndCheckEpoch();

        // Having the metadata manager registered on the collection sharding state is what indicates
        // that a chunk on that collection is being migrated. With an active migration, write
        // operations require the cloner to be present in order to track changes to the chunk which
        // needs to be transmitted to the recipient.
        _cloneDriver = std::make_unique<MigrationChunkClonerSourceLegacy>(
            _args, metadata.getKeyPattern(), _donorConnStr, _recipientHost);

        AutoGetCollection autoColl(_opCtx,
                                   getNss(),
                                   replEnabled ? MODE_IX : MODE_X,
                                   AutoGetCollectionViewMode::kViewsForbidden,
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                       Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));

        auto csr = CollectionShardingRuntime::get(_opCtx, getNss());
        auto lockedCsr = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
        invariant(nullptr == std::exchange(msmForCsr(csr), this));

        _coordinator = std::make_unique<migrationutil::MigrationCoordinator>(
            _cloneDriver->getSessionId(),
            _args.getFromShardId(),
            _args.getToShardId(),
            getNss(),
            *_collectionUUID,
            ChunkRange(_args.getMinKey(), _args.getMaxKey()),
            _chunkVersion,
            _args.getWaitForDelete());

        _state = kCloning;
    }

    if (replEnabled) {
        auto const readConcernArgs = repl::ReadConcernArgs(
            replCoord->getMyLastAppliedOpTime(), repl::ReadConcernLevel::kLocalReadConcern);

        auto waitForReadConcernStatus =
            waitForReadConcern(_opCtx, readConcernArgs, StringData(), false);
        if (!waitForReadConcernStatus.isOK()) {
            return waitForReadConcernStatus;
        }
        setPrepareConflictBehaviorForReadConcern(
            _opCtx, readConcernArgs, PrepareConflictBehavior::kEnforce);
    }

    _coordinator->startMigration(_opCtx);

    Status startCloneStatus = _cloneDriver->startClone(_opCtx,
                                                       _coordinator->getMigrationId(),
                                                       _coordinator->getLsid(),
                                                       _coordinator->getTxnNumber());
    if (!startCloneStatus.isOK()) {
        return startCloneStatus;
    }

    scopedGuard.dismiss();
    return Status::OK();
}

Status MigrationSourceManager::awaitToCatchUp() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloning);
    ScopeGuard scopedGuard([&] { cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    // Block until the cloner deems it appropriate to enter the critical section.
    Status catchUpStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        _opCtx, kMaxWaitToEnterCriticalSectionTimeout);
    if (!catchUpStatus.isOK()) {
        return catchUpStatus;
    }

    _state = kCloneCaughtUp;
    scopedGuard.dismiss();
    return Status::OK();
}

Status MigrationSourceManager::enterCriticalSection() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    ScopeGuard scopedGuard([&] { cleanupOnError(); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    _notifyChangeStreamsOnRecipientFirstChunk(_getCurrentMetadataAndCheckEpoch());

    // Mark the shard as running critical operation, which requires recovery on crash.
    //
    // NOTE: The 'migrateChunkToNewShard' oplog message written by the above call to
    // '_notifyChangeStreamsOnRecipientFirstChunk' depends on this majority write to carry its local
    // write to majority committed.
    Status status = ShardingStateRecovery::startMetadataOp(_opCtx);
    if (!status.isOK()) {
        return status;
    }

    LOGV2_DEBUG_OPTIONS(4817402,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Starting critical section",
                        "migrationId"_attr = _coordinator->getMigrationId());

    _critSec.emplace(_opCtx, _args.getNss(), _critSecReason);

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This is will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the migration config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus = shardmetadatautil::updateShardCollectionsEntry(
        _opCtx,
        BSON(ShardCollectionType::kNssFieldName << getNss().ns()),
        BSON("$inc" << BSON(ShardCollectionType::kEnterCriticalSectionCounterFieldName << 1)),
        false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    LOGV2(22017,
          "Migration successfully entered critical section",
          "migrationId"_attr = _coordinator->getMigrationId());

    scopedGuard.dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkOnRecipient() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    ScopeGuard scopedGuard([&] { cleanupOnError(); });

    // Tell the recipient shard to fetch the latest changes.
    auto commitCloneStatus = _cloneDriver->commitClone(_opCtx);

    if (MONGO_unlikely(failMigrationCommit.shouldFail()) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    if (!commitCloneStatus.isOK()) {
        return commitCloneStatus.getStatus().withContext("commit clone failed");
    }

    _recipientCloneCounts = commitCloneStatus.getValue()["counts"].Obj().getOwned();

    _state = kCloneCompleted;
    scopedGuard.dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkMetadataOnConfig() {
    invariant(!_opCtx->lockState()->isLocked());
    invariant(_state == kCloneCompleted);
    ScopeGuard scopedGuard([&] { cleanupOnError(); });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = _getCurrentMetadataAndCheckEpoch();

        ChunkType migratedChunkType;
        migratedChunkType.setMin(_args.getMinKey());
        migratedChunkType.setMax(_args.getMaxKey());
        migratedChunkType.setVersion(_chunkVersion);

        const auto currentTime = VectorClock::get(_opCtx)->getTime();
        CommitChunkMigrationRequest::appendAsCommand(&builder,
                                                     getNss(),
                                                     _args.getFromShardId(),
                                                     _args.getToShardId(),
                                                     migratedChunkType,
                                                     metadata.getCollVersion(),
                                                     currentTime.clusterTime().asTimestamp());

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
            AutoGetCollection autoColl(_opCtx, getNss(), MODE_IX);
            CollectionShardingRuntime::get(_opCtx, getNss())->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        // Best-effort recover of the shard version.
        onShardVersionMismatchNoExcept(_opCtx, getNss(), boost::none).ignore();
        return migrationCommitStatus;
    }

    hangBeforePostMigrationCommitRefresh.pauseWhileSet();

    try {
        LOGV2_DEBUG_OPTIONS(4817404,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Starting post-migration commit refresh on the shard",
                            "migrationId"_attr = _coordinator->getMigrationId());

        forceShardFilteringMetadataRefresh(_opCtx, getNss());

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
            AutoGetCollection autoColl(_opCtx, getNss(), MODE_IX);
            CollectionShardingRuntime::get(_opCtx, getNss())->clearFilteringMetadata(_opCtx);
        }
        scopedGuard.dismiss();
        _cleanup(false);
        // Best-effort recover of the shard version.
        onShardVersionMismatchNoExcept(_opCtx, getNss(), boost::none).ignore();
        return ex.toStatus();
    }

    // Migration succeeded

    const auto refreshedMetadata = _getCurrentMetadataAndCheckEpoch();

    LOGV2(22018,
          "Migration succeeded and updated collection version to {updatedCollectionVersion}",
          "Migration succeeded and updated collection version",
          "updatedCollectionVersion"_attr = refreshedMetadata.getCollVersion(),
          "migrationId"_attr = _coordinator->getMigrationId());

    // If the migration has succeeded, clear the BucketCatalog so that the buckets that got migrated
    // out are no longer updatable.
    if (getNss().isTimeseriesBucketsCollection()) {
        auto& bucketCatalog = BucketCatalog::get(_opCtx);
        bucketCatalog.clear(getNss().getTimeseriesViewNamespace());
    }

    _coordinator->setMigrationDecision(DecisionEnum::kCommitted);

    hangBeforeLeavingCriticalSection.pauseWhileSet();

    scopedGuard.dismiss();

    _stats.totalCriticalSectionCommitTimeMillis.addAndFetch(t.millis());

    // Exit the critical section and ensure that all the necessary state is fully persisted before
    // scheduling orphan cleanup.
    _cleanup(true);

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.commit",
        getNss().ns(),
        BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                   << _args.getFromShardId() << "to" << _args.getToShardId() << "counts"
                   << _recipientCloneCounts),
        ShardingCatalogClient::kMajorityWriteConcern);

    const ChunkRange range(_args.getMinKey(), _args.getMaxKey());

    if (!MONGO_unlikely(doNotRefreshRecipientAfterCommit.shouldFail())) {
        // Best-effort make the recipient refresh its routing table to the new collection
        // version.
        refreshRecipientRoutingTable(
            _opCtx, getNss(), _recipientHost, refreshedMetadata.getCollVersion());
    }

    std::string orphanedRangeCleanUpErrMsg = str::stream()
        << "Moved chunks successfully but failed to clean up " << getNss().ns() << " range "
        << redact(range.toString()) << " due to: ";

    if (_args.getWaitForDelete()) {
        LOGV2(22019,
              "Waiting for migration cleanup after chunk commit for the namespace {namespace} "
              "and range {range}",
              "Waiting for migration cleanup after chunk commit",
              "namespace"_attr = getNss().ns(),
              "range"_attr = redact(range.toString()),
              "migrationId"_attr = _coordinator->getMigrationId());

        Status deleteStatus = _cleanupCompleteFuture
            ? _cleanupCompleteFuture->getNoThrow(_opCtx)
            : Status(ErrorCodes::Error(5089002),
                     "Not honouring the 'waitForDelete' request because migration coordinator "
                     "cleanup didn't succeed");
        if (!deleteStatus.isOK()) {
            return {ErrorCodes::OrphanedRangeCleanUpFailed,
                    orphanedRangeCleanUpErrMsg + redact(deleteStatus)};
        }
    }

    return Status::OK();
}

void MigrationSourceManager::cleanupOnError() {
    if (_state == kDone) {
        return;
    }

    ShardingLogging::get(_opCtx)->logChange(
        _opCtx,
        "moveChunk.error",
        getNss().ns(),
        BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                   << _args.getFromShardId() << "to" << _args.getToShardId()),
        ShardingCatalogClient::kMajorityWriteConcern);

    _cleanup(true);
}

void MigrationSourceManager::abortDueToConflictingIndexOperation(OperationContext* opCtx) {
    // Index operations sent in the 4.4 protocol from internal clients are versioned and block
    // behind both phases of the critical section, so there should never be an active critical
    // section in this case.
    dassert(!_critSec || !opCtx->getClient()->session() ||
            !(opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient));
    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    _opCtx->markKilled();
    _stats.countDonorMoveChunkAbortConflictingIndexOperation.addAndFetch(1);
}

CollectionMetadata MigrationSourceManager::_getCurrentMetadataAndCheckEpoch() {
    auto metadata = [&] {
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, getNss(), MODE_IS);
        auto* const css = CollectionShardingRuntime::get(_opCtx, getNss());

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

void MigrationSourceManager::_notifyChangeStreamsOnRecipientFirstChunk(
    const CollectionMetadata& metadata) {
    // If this is not the first donation, there is nothing to be done
    if (metadata.getChunkManager()->getVersion(_args.getToShardId()).isSet())
        return;

    const std::string dbgMessage = str::stream()
        << "Migrating chunk from shard " << _args.getFromShardId() << " to shard "
        << _args.getToShardId() << " with no chunks for this collection";

    // The message expected by change streams
    const auto o2Message = BSON("type"
                                << "migrateChunkToNewShard"
                                << "from" << _args.getFromShardId() << "to"
                                << _args.getToShardId());

    auto const serviceContext = _opCtx->getClient()->getServiceContext();

    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetOplog oplogWrite(_opCtx, OplogAccessMode::kWrite);
    writeConflictRetry(
        _opCtx, "migrateChunkToNewShard", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork uow(_opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(_opCtx,
                                                                 getNss(),
                                                                 *_collectionUUID,
                                                                 BSON("msg" << dbgMessage),
                                                                 o2Message,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none,
                                                                 boost::none);
            uow.commit();
        });
}

void MigrationSourceManager::_cleanup(bool completeMigration) noexcept {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state and exit the migration critical section.
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, getNss(), MODE_IX);
        auto* const csr = CollectionShardingRuntime::get(_opCtx, getNss());
        auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

        if (_state != kCreated) {
            invariant(msmForCsr(csr));
            invariant(_cloneDriver);
        }

        // While we are in kCreated, the MigrationSourceManager may or may not be already be
        // installed on the CollectionShardingRuntime.
        if (_state != kCreated || (_state == kCreated && msmForCsr(csr))) {
            auto oldMsmOnCsr = std::exchange(msmForCsr(csr), nullptr);
            invariant(this == oldMsmOnCsr);
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
                CatalogCacheLoader::get(newOpCtx).waitForCollectionFlush(newOpCtx, getNss());

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
                      "chunkMigrationRequestParameters"_attr = redact(_args.toString()),
                      "error"_attr = redact(ex),
                      "migrationId"_attr = _coordinator->getMigrationId());
        // Something went really wrong when completing the migration just unset the metadata and let
        // the next op to recover.
        UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
        AutoGetCollection autoColl(_opCtx, getNss(), MODE_IX);
        CollectionShardingRuntime::get(_opCtx, getNss())->clearFilteringMetadata(_opCtx);
    }
}

BSONObj MigrationSourceManager::getMigrationStatusReport() const {
    return migrationutil::makeMigrationStatusDocument(getNss(),
                                                      _args.getFromShardId(),
                                                      _args.getToShardId(),
                                                      true,
                                                      _args.getMinKey(),
                                                      _args.getMaxKey());
}

}  // namespace mongo
