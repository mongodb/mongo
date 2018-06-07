/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_source_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/commit_chunk_migration_request_type.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using namespace shardmetadatautil;

namespace {

const auto msmForCss = CollectionShardingState::declareDecoration<MigrationSourceManager*>();

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);
const char kMigratedChunkVersionField[] = "migratedChunkVersion";
const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

/**
 * Best-effort attempt to ensure the recipient shard has refreshed its routing table to
 * 'newCollVersion'. Fires and forgets an asychronous remote setShardVersion command.
 */
void refreshRecipientRoutingTable(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  ShardId toShard,
                                  const HostAndPort& toShardHost,
                                  const ChunkVersion& newCollVersion) {
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString(),
        toShard,
        ConnectionString(toShardHost),
        nss,
        newCollVersion,
        false);

    const executor::RemoteCommandRequest request(
        toShardHost,
        NamespaceString::kAdminDb.toString(),
        ssv.toBSON(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toContainingBSON(),
        opCtx,
        executor::RemoteCommandRequest::kNoTimeout);

    executor::TaskExecutor* const executor =
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto noOp = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    executor->scheduleRemoteCommand(request, noOp).getStatus().ignore();
}

Status checkCollectionEpochMatches(const ScopedCollectionMetadata& metadata, OID expectedEpoch) {
    if (metadata && metadata->getCollVersion().epoch() == expectedEpoch)
        return Status::OK();

    return {ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "The collection was dropped or recreated since the migration began. "
                          << "Expected collection epoch: "
                          << expectedEpoch.toString()
                          << ", but found: "
                          << (metadata ? metadata->getCollVersion().epoch().toString()
                                       : "unsharded collection.")};
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(doNotRefreshRecipientAfterCommit);
MONGO_FAIL_POINT_DEFINE(failMigrationCommit);
MONGO_FAIL_POINT_DEFINE(hangBeforeLeavingCriticalSection);
MONGO_FAIL_POINT_DEFINE(migrationCommitNetworkError);

MigrationSourceManager* MigrationSourceManager::get(CollectionShardingState& css) {
    return msmForCss(css);
}

MigrationSourceManager::MigrationSourceManager(OperationContext* opCtx,
                                               MoveChunkRequest request,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
    : _args(std::move(request)),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _stats(ShardingStatistics::get(opCtx)) {
    invariant(!opCtx->lockState()->isLocked());

    // Disallow moving a chunk to ourselves
    uassert(ErrorCodes::InvalidOptions,
            "Destination shard cannot be the same as source",
            _args.getFromShardId() != _args.getToShardId());

    log() << "Starting chunk migration " << redact(_args.toString())
          << " with expected collection version epoch " << _args.getVersionEpoch();

    // Force refresh of the metadata to ensure we have the latest
    forceShardFilteringMetadataRefresh(opCtx, getNss());

    // Snapshot the committed metadata from the time the migration starts
    const auto collectionMetadataAndUUID = [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        uassert(ErrorCodes::InvalidOptions,
                "cannot move chunks for a collection that doesn't exist",
                autoColl.getCollection());

        boost::optional<UUID> collectionUUID;
        if (autoColl.getCollection()->uuid()) {
            collectionUUID = autoColl.getCollection()->uuid().value();
        }

        auto metadata = CollectionShardingState::get(opCtx, getNss())->getMetadata(opCtx);
        uassert(ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "cannot move chunks for an unsharded collection",
                metadata);

        return std::make_tuple(std::move(metadata), std::move(collectionUUID));
    }();

    const auto& collectionMetadata = std::get<0>(collectionMetadataAndUUID);

    const auto collectionVersion = collectionMetadata->getCollVersion();
    const auto shardVersion = collectionMetadata->getShardVersion();

    // If the shard major version is zero, this means we do not have any chunks locally to migrate
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because the shard doesn't contain any chunks",
            shardVersion.majorVersion() > 0);

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "cannot move chunk " << _args.toString()
                          << " because collection may have been dropped. "
                          << "current epoch: "
                          << collectionVersion.epoch()
                          << ", cmd epoch: "
                          << _args.getVersionEpoch(),
            _args.getVersionEpoch() == collectionVersion.epoch());

    ChunkType chunkToMove;
    chunkToMove.setMin(_args.getMinKey());
    chunkToMove.setMax(_args.getMaxKey());

    uassertStatusOKWithContext(collectionMetadata->checkChunkIsValid(chunkToMove),
                               str::stream() << "Unable to move chunk with arguments '"
                                             << redact(_args.toString()));

    _collectionEpoch = collectionVersion.epoch();
    _collectionUuid = std::get<1>(collectionMetadataAndUUID);
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
    _stats.totalDonorMoveChunkTimeMillis.addAndFetch(_entireOpTimer.millis());
}

NamespaceString MigrationSourceManager::getNss() const {
    return _args.getNss();
}

Status MigrationSourceManager::startClone(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });
    _stats.countDonorMoveChunkStarted.addAndFetch(1);

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.start",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    _cloneAndCommitTimer.reset();

    {
        // Register for notifications from the replication subsystem
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
        auto css = CollectionShardingState::get(opCtx, getNss());

        const auto metadata = css->getMetadata(opCtx);
        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        // Having the metadata manager registered on the collection sharding state is what indicates
        // that a chunk on that collection is being migrated. With an active migration, write
        // operations require the cloner to be present in order to track changes to the chunk which
        // needs to be transmitted to the recipient.
        _cloneDriver = stdx::make_unique<MigrationChunkClonerSourceLegacy>(
            _args, metadata->getKeyPattern(), _donorConnStr, _recipientHost);

        invariant(nullptr == std::exchange(msmForCss(css), this));
    }

    Status startCloneStatus = _cloneDriver->startClone(opCtx);
    if (!startCloneStatus.isOK()) {
        return startCloneStatus;
    }

    _state = kCloning;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::awaitToCatchUp(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloning);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    // Block until the cloner deems it appropriate to enter the critical section.
    Status catchUpStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        opCtx, kMaxWaitToEnterCriticalSectionTimeout);
    if (!catchUpStatus.isOK()) {
        return catchUpStatus;
    }

    _state = kCloneCaughtUp;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::enterCriticalSection(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });
    _stats.totalDonorChunkCloneTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());
    _cloneAndCommitTimer.reset();

    {
        const auto metadata = [&] {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);
            return CollectionShardingState::get(opCtx, _args.getNss())->getMetadata(opCtx);
        }();

        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        _notifyChangeStreamsOnRecipientFirstChunk(opCtx, metadata);
    }

    // Mark the shard as running critical operation, which requires recovery on crash.
    //
    // NOTE: The 'migrateChunkToNewShard' oplog message written by the above call to
    // '_notifyChangeStreamsOnRecipientFirstChunk' depends on this majority write to carry its local
    // write to majority committed.
    Status status = ShardingStateRecovery::startMetadataOp(opCtx);
    if (!status.isOK()) {
        return status;
    }

    {
        // The critical section must be entered with collection X lock in order to ensure there are
        // no writes which could have entered and passed the version check just before we entered
        // the crticial section, but managed to complete after we left it.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        CollectionShardingState::get(opCtx, _args.getNss())
            ->enterCriticalSectionCatchUpPhase(opCtx);
    }

    _state = kCriticalSection;

    // Persist a signal to secondaries that we've entered the critical section. This is will cause
    // secondaries to refresh their routing table when next accessed, which will block behind the
    // critical section. This ensures causal consistency by preventing a stale mongos with a cluster
    // time inclusive of the migration config commit update from accessing secondary data.
    // Note: this write must occur after the critSec flag is set, to ensure the secondary refresh
    // will stall behind the flag.
    Status signalStatus =
        updateShardCollectionsEntry(opCtx,
                                    BSON(ShardCollectionType::ns() << getNss().ns()),
                                    BSONObj(),
                                    BSON(ShardCollectionType::enterCriticalSectionCounter() << 1),
                                    false /*upsert*/);
    if (!signalStatus.isOK()) {
        return {
            ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for secondaries due to: "
                          << signalStatus.toString()};
    }

    log() << "Migration successfully entered critical section";

    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkOnRecipient(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // Tell the recipient shard to fetch the latest changes.
    auto commitCloneStatus = _cloneDriver->commitClone(opCtx);

    if (MONGO_FAIL_POINT(failMigrationCommit) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    if (!commitCloneStatus.isOK()) {
        return commitCloneStatus.getStatus().withContext("commit clone failed");
    }

    _recipientCloneCounts = commitCloneStatus.getValue()["counts"].Obj().getOwned();

    _state = kCloneCompleted;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkMetadataOnConfig(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(_state == kCloneCompleted);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(opCtx); });

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    BSONObjBuilder builder;

    {
        const auto metadata = [&] {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            AutoGetCollection autoColl(opCtx, _args.getNss(), MODE_IS);
            return CollectionShardingState::get(opCtx, _args.getNss())->getMetadata(opCtx);
        }();

        Status status = checkCollectionEpochMatches(metadata, _collectionEpoch);
        if (!status.isOK())
            return status;

        ChunkType migratedChunkType;
        migratedChunkType.setMin(_args.getMinKey());
        migratedChunkType.setMax(_args.getMaxKey());

        CommitChunkMigrationRequest::appendAsCommand(
            &builder,
            getNss(),
            _args.getFromShardId(),
            _args.getToShardId(),
            migratedChunkType,
            metadata->getCollVersion(),
            LogicalClock::get(opCtx)->getClusterTime().asTimestamp());

        builder.append(kWriteConcernField, kMajorityWriteConcern.toBSON());
    }

    // Read operations must begin to wait on the critical section just before we send the commit
    // operation to the config server
    {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
        CollectionShardingState::get(opCtx, _args.getNss())->enterCriticalSectionCommitPhase(opCtx);
    }

    Timer t;

    auto commitChunkMigrationResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            builder.obj(),
            Shard::RetryPolicy::kIdempotent);

    if (MONGO_FAIL_POINT(migrationCommitNetworkError)) {
        commitChunkMigrationResponse = Status(
            ErrorCodes::InternalError, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    Status migrationCommitStatus = commitChunkMigrationResponse.getStatus();
    if (migrationCommitStatus.isOK()) {
        migrationCommitStatus = commitChunkMigrationResponse.getValue().commandStatus;
        if (migrationCommitStatus.isOK()) {
            migrationCommitStatus = commitChunkMigrationResponse.getValue().writeConcernStatus;
        }
    }

    if (!migrationCommitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _configsvrCommitChunkMigration may have
        // done
        log() << "Error occurred while committing the migration. Performing a majority write "
                 "against the config server to obtain its latest optime"
              << causedBy(redact(migrationCommitStatus));

        Status status = Grid::get(opCtx)->catalogClient()->logChange(
            opCtx,
            "moveChunk.validating",
            getNss().ns(),
            BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                       << _args.getFromShardId()
                       << "to"
                       << _args.getToShardId()),
            ShardingCatalogClient::kMajorityWriteConcern);

        if ((ErrorCodes::isInterruption(status.code()) ||
             ErrorCodes::isShutdownError(status.code()) ||
             status == ErrorCodes::CallbackCanceled) &&
            globalInShutdownDeprecated()) {
            // Since the server is already doing a clean shutdown, this call will just join the
            // previous shutdown call
            shutdown(waitForShutdown());
        }

        // If we failed to get the latest config optime because we stepped down as primary, then it
        // is safe to fail without crashing because the new primary will fetch the latest optime
        // when it recovers the sharding state recovery document, as long as we also clear the
        // metadata for this collection, forcing subsequent callers to do a full refresh. Check if
        // this node can accept writes for this collection as a proxy for it being primary.
        if (!status.isOK()) {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
            if (!repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, getNss())) {
                CollectionShardingState::get(opCtx, getNss())->refreshMetadata(opCtx, nullptr);
                uassertStatusOK(status.withContext(
                    str::stream() << "Unable to verify migration commit for chunk: "
                                  << redact(_args.toString())
                                  << " because the node's replication role changed. Metadata "
                                     "was cleared for: "
                                  << getNss().ns()
                                  << ", so it will get a full refresh when accessed again."));
            }
        }

        fassert(40137,
                status.withContext(
                    str::stream() << "Failed to commit migration for chunk " << _args.toString()
                                  << " due to "
                                  << redact(migrationCommitStatus)
                                  << ". Updating the optime with a write before refreshing the "
                                  << "metadata also failed"));
    }

    // Do a best effort attempt to incrementally refresh the metadata before leaving the critical
    // section. It is okay if the refresh fails because that will cause the metadata to be cleared
    // and subsequent callers will try to do a full refresh.
    const auto refreshStatus = [&] {
        try {
            forceShardFilteringMetadataRefresh(opCtx, getNss());
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    if (!refreshStatus.isOK()) {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);

        CollectionShardingState::get(opCtx, getNss())->refreshMetadata(opCtx, nullptr);

        log() << "Failed to refresh metadata after a "
              << (migrationCommitStatus.isOK() ? "failed commit attempt" : "successful commit")
              << ". Metadata was cleared so it will get a full refresh when accessed again."
              << causedBy(redact(refreshStatus));

        // migrationCommitStatus may be OK or an error. The migration is considered a success at
        // this point if the commit succeeded. The metadata refresh either occurred or the metadata
        // was safely cleared.
        return migrationCommitStatus.withContext(
            str::stream() << "Orphaned range not cleaned up. Failed to refresh metadata after"
                             " migration commit due to '"
                          << refreshStatus.toString()
                          << "' after commit failed");
    }

    auto refreshedMetadata = [&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        return CollectionShardingState::get(opCtx, getNss())->getMetadata(opCtx);
    }();

    if (!refreshedMetadata) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Chunk move failed because collection '" << getNss().ns()
                              << "' is no longer sharded. The migration commit error was: "
                              << migrationCommitStatus.toString()};
    }

    if (refreshedMetadata->keyBelongsToMe(_args.getMinKey())) {
        // The chunk modification was not applied, so report the original error
        return migrationCommitStatus.withContext("Chunk move was not successful");
    }

    // Migration succeeded
    log() << "Migration succeeded and updated collection version to "
          << refreshedMetadata->getCollVersion();

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    scopedGuard.Dismiss();

    _stats.totalCriticalSectionCommitTimeMillis.addAndFetch(t.millis());

    // Exit the critical section and ensure that all the necessary state is fully persisted before
    // scheduling orphan cleanup.
    _cleanup(opCtx);

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.commit",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()
                               << "counts"
                               << _recipientCloneCounts),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    const ChunkRange range(_args.getMinKey(), _args.getMaxKey());

    auto notification = [&] {
        auto const whenToClean = _args.getWaitForDelete() ? CollectionShardingState::kNow
                                                          : CollectionShardingState::kDelayed;
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IS);
        return CollectionShardingState::get(opCtx, getNss())->cleanUpRange(range, whenToClean);
    }();

    if (!MONGO_FAIL_POINT(doNotRefreshRecipientAfterCommit)) {
        // Best-effort make the recipient refresh its routing table to the new collection version.
        refreshRecipientRoutingTable(opCtx,
                                     getNss(),
                                     _args.getToShardId(),
                                     _recipientHost,
                                     refreshedMetadata->getCollVersion());
    }

    if (_args.getWaitForDelete()) {
        log() << "Waiting for cleanup of " << getNss().ns() << " range "
              << redact(range.toString());
        return notification.waitStatus(opCtx);
    }

    if (notification.ready() && !notification.waitStatus(opCtx).isOK()) {
        warning() << "Failed to initiate cleanup of " << getNss().ns() << " range "
                  << redact(range.toString())
                  << " due to: " << redact(notification.waitStatus(opCtx));
    } else {
        log() << "Leaving cleanup of " << getNss().ns() << " range " << redact(range.toString())
              << " to complete in background";
        notification.abandon();
    }

    return Status::OK();
}

void MigrationSourceManager::cleanupOnError(OperationContext* opCtx) {
    if (_state == kDone) {
        return;
    }

    Grid::get(opCtx)
        ->catalogClient()
        ->logChange(opCtx,
                    "moveChunk.error",
                    getNss().ns(),
                    BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                               << _args.getFromShardId()
                               << "to"
                               << _args.getToShardId()),
                    ShardingCatalogClient::kMajorityWriteConcern)
        .ignore();

    try {
        _cleanup(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::NotMasterError>& ex) {
        warning() << "Failed to clean up migration: " << redact(_args.toString())
                  << "due to: " << redact(ex);
    }
}

void MigrationSourceManager::_notifyChangeStreamsOnRecipientFirstChunk(
    OperationContext* opCtx, const ScopedCollectionMetadata& metadata) {
    // If this is not the first donation, there is nothing to be done
    if (metadata->getChunkManager()->getVersion(_args.getToShardId()).isSet())
        return;

    const std::string dbgMessage = str::stream()
        << "Migrating chunk from shard " << _args.getFromShardId() << " to shard "
        << _args.getToShardId() << " with no chunks for this collection";

    // The message expected by change streams
    const auto o2Message = BSON("type"
                                << "migrateChunkToNewShard"
                                << "from"
                                << _args.getFromShardId()
                                << "to"
                                << _args.getToShardId());

    auto const serviceContext = opCtx->getClient()->getServiceContext();

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    AutoGetCollection autoColl(opCtx, NamespaceString::kRsOplogNamespace, MODE_IX);
    writeConflictRetry(
        opCtx, "migrateChunkToNewShard", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork uow(opCtx);
            serviceContext->getOpObserver()->onInternalOpMessage(
                opCtx, getNss(), _collectionUuid, BSON("msg" << dbgMessage), o2Message);
            uow.commit();
        });
}

void MigrationSourceManager::_cleanup(OperationContext* opCtx) {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state and exit the migration critical section.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, getNss(), MODE_IX, MODE_X);
        auto css = CollectionShardingState::get(opCtx, getNss());

        invariant(this == std::exchange(msmForCss(css), nullptr));
        css->exitCriticalSection(opCtx);
        return std::move(_cloneDriver);
    }();

    // The cleanup operations below are potentially blocking or acquire other locks, so perform them
    // outside of the collection X lock

    if (cloneDriver) {
        cloneDriver->cancelClone(opCtx);
    }

    if (_state == kCriticalSection || _state == kCloneCompleted) {
        _stats.totalCriticalSectionTimeMillis.addAndFetch(_cloneAndCommitTimer.millis());

        // NOTE: The order of the operations below is important and the comments explain the
        // reasoning behind it

        // Wait for the updates to the cache of the routing table to be fully written to disk before
        // clearing the 'minOpTime recovery' document. This way, we ensure that all nodes from a
        // shard, which donated a chunk will always be at the shard version of the last migration it
        // performed.
        //
        // If the metadata is not persisted before clearing the 'inMigration' flag below, it is
        // possible that the persisted metadata is rolled back after step down, but the write which
        // cleared the 'inMigration' flag is not, a secondary node will report itself at an older
        // shard version.
        CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, getNss());

        // Clear the 'minOpTime recovery' document so that the next time a node from this shard
        // becomes a primary, it won't have to recover the config server optime.
        ShardingStateRecovery::endMetadataOp(opCtx);
    }

    _state = kDone;
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
