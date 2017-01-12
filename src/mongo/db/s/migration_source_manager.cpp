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
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/commit_chunk_migration_request_type.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

// Wait at most this much time for the recipient to catch up sufficiently so critical section can be
// entered
const Hours kMaxWaitToEnterCriticalSectionTimeout(6);
const char kMigratedChunkVersionField[] = "migratedChunkVersion";
const char kControlChunkVersionField[] = "controlChunkVersion";
const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

}  // namespace

MONGO_FP_DECLARE(migrationCommitNetworkError);
MONGO_FP_DECLARE(failMigrationCommit);
MONGO_FP_DECLARE(hangBeforeLeavingCriticalSection);

MigrationSourceManager::MigrationSourceManager(OperationContext* txn,
                                               MoveChunkRequest request,
                                               ConnectionString donorConnStr,
                                               HostAndPort recipientHost)
    : _args(std::move(request)),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _startTime() {
    invariant(!txn->lockState()->isLocked());

    // Disallow moving a chunk to ourselves
    uassert(ErrorCodes::InvalidOptions,
            "Destination shard cannot be the same as source",
            _args.getFromShardId() != _args.getToShardId());

    log() << "Starting chunk migration " << redact(_args.toString())
          << " with expected collection version epoch" << _args.getVersionEpoch();

    // Now that the collection is locked, snapshot the metadata and fetch the latest versions
    ShardingState* const shardingState = ShardingState::get(txn);

    ChunkVersion shardVersion;

    Status refreshStatus = shardingState->refreshMetadataNow(txn, getNss(), &shardVersion);
    if (!refreshStatus.isOK()) {
        uasserted(refreshStatus.code(),
                  str::stream() << "cannot start migrate of chunk " << _args.toString()
                                << " due to "
                                << refreshStatus.toString());
    }

    if (shardVersion.majorVersion() == 0) {
        // If the major version is zero, this means we do not have any chunks locally to migrate in
        // the first place
        uasserted(ErrorCodes::IncompatibleShardingMetadata,
                  str::stream() << "cannot start migrate of chunk " << _args.toString()
                                << " with zero shard version");
    }

    // Snapshot the committed metadata from the time the migration starts
    {
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetCollection autoColl(txn, getNss(), MODE_IS);

        _collectionMetadata = CollectionShardingState::get(txn, getNss())->getMetadata();
        _keyPattern = _collectionMetadata->getKeyPattern();
    }

    const ChunkVersion collectionVersion = _collectionMetadata->getCollVersion();

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "cannot move chunk " << redact(_args.toString())
                          << " because collection may have been dropped. "
                          << "current epoch: "
                          << collectionVersion.epoch()
                          << ", cmd epoch: "
                          << _args.getVersionEpoch(),
            _args.getVersionEpoch() == collectionVersion.epoch());

    // With nonzero shard version, we must have a coll version >= our shard version
    invariant(collectionVersion >= shardVersion);

    // With nonzero shard version, we must have a shard key
    invariant(!_collectionMetadata->getKeyPattern().isEmpty());

    ChunkType chunkToMove;
    chunkToMove.setMin(_args.getMinKey());
    chunkToMove.setMax(_args.getMaxKey());

    Status chunkValidateStatus = _collectionMetadata->checkChunkIsValid(chunkToMove);
    if (!chunkValidateStatus.isOK()) {
        uasserted(chunkValidateStatus.code(),
                  str::stream() << "Unable to move chunk with arguments '"
                                << redact(_args.toString())
                                << "' due to error "
                                << redact(chunkValidateStatus.reason()));
    }
}

MigrationSourceManager::~MigrationSourceManager() {
    invariant(!_cloneDriver);
}

NamespaceString MigrationSourceManager::getNss() const {
    return _args.getNss();
}

Status MigrationSourceManager::startClone(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCreated);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    grid.catalogClient(txn)->logChange(txn,
                                       "moveChunk.start",
                                       getNss().ns(),
                                       BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey()
                                                  << "from"
                                                  << _args.getFromShardId()
                                                  << "to"
                                                  << _args.getToShardId()),
                                       ShardingCatalogClient::kMajorityWriteConcern);

    _cloneDriver = stdx::make_unique<MigrationChunkClonerSourceLegacy>(
        _args, _collectionMetadata->getKeyPattern(), _donorConnStr, _recipientHost);

    {
        // Register for notifications from the replication subsystem
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, getNss().ns());
        css->setMigrationSourceManager(txn, this);
    }

    Status startCloneStatus = _cloneDriver->startClone(txn);
    if (!startCloneStatus.isOK()) {
        return startCloneStatus;
    }

    _state = kCloning;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::awaitToCatchUp(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCloning);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Block until the cloner deems it appropriate to enter the critical section.
    Status catchUpStatus = _cloneDriver->awaitUntilCriticalSectionIsAppropriate(
        txn, kMaxWaitToEnterCriticalSectionTimeout);
    if (!catchUpStatus.isOK()) {
        return catchUpStatus;
    }

    _state = kCloneCaughtUp;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::enterCriticalSection(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCloneCaughtUp);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Mark the shard as running critical operation, which requires recovery on crash
    Status status = ShardingStateRecovery::startMetadataOp(txn);
    if (!status.isOK()) {
        return status;
    }

    {
        // The critical section must be entered with collection X lock in order to ensure there are
        // no writes which could have entered and passed the version check just before we entered
        // the crticial section, but managed to complete after we left it.
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, getNss().ns());
        auto metadata = css->getMetadata();
        if (!metadata ||
            !metadata->getCollVersion().equals(_collectionMetadata->getCollVersion())) {
            return {ErrorCodes::IncompatibleShardingMetadata,
                    str::stream()
                        << "Sharding metadata changed while holding distributed lock. Expected: "
                        << _collectionMetadata->getCollVersion().toString()
                        << ", but found: "
                        << (metadata ? metadata->getCollVersion().toString()
                                     : "unsharded collection.")};
        }

        // IMPORTANT: After this line, the critical section is in place and needs to be signaled
        _critSecSignal = std::make_shared<Notification<void>>();
    }

    log() << "Migration successfully entered critical section";

    _state = kCriticalSection;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkOnRecipient(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCriticalSection);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    // Tell the recipient shard to fetch the latest changes.
    Status commitCloneStatus = _cloneDriver->commitClone(txn);

    if (MONGO_FAIL_POINT(failMigrationCommit) && commitCloneStatus.isOK()) {
        commitCloneStatus = {ErrorCodes::InternalError,
                             "Failing _recvChunkCommit due to failpoint."};
    }

    if (!commitCloneStatus.isOK()) {
        return {commitCloneStatus.code(),
                str::stream() << "commit clone failed due to " << commitCloneStatus.toString()};
    }

    _state = kCloneCompleted;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitChunkMetadataOnConfig(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());
    invariant(_state == kCloneCompleted);
    auto scopedGuard = MakeGuard([&] { cleanupOnError(txn); });

    ChunkType migratedChunkType;
    migratedChunkType.setMin(_args.getMinKey());
    migratedChunkType.setMax(_args.getMaxKey());

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    boost::optional<ChunkType> controlChunkType = boost::none;
    if (_collectionMetadata->getNumChunks() > 1) {
        ChunkType differentChunk;
        invariant(_collectionMetadata->getDifferentChunk(_args.getMinKey(), &differentChunk));
        invariant(differentChunk.getMin().woCompare(_args.getMinKey()) != 0);
        controlChunkType = std::move(differentChunk);
    } else {
        log() << "Moving last chunk for the collection out";
    }

    BSONObjBuilder builder;
    CommitChunkMigrationRequest::appendAsCommand(&builder,
                                                 getNss(),
                                                 _args.getFromShardId(),
                                                 _args.getToShardId(),
                                                 migratedChunkType,
                                                 controlChunkType,
                                                 _collectionMetadata->getCollVersion());

    builder.append(kWriteConcernField, kMajorityWriteConcern.toBSON());

    auto commitChunkMigrationResponse =
        grid.shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            builder.obj(),
            Shard::RetryPolicy::kIdempotent);

    if (MONGO_FAIL_POINT(migrationCommitNetworkError)) {
        commitChunkMigrationResponse = Status(
            ErrorCodes::InternalError, "Failpoint 'migrationCommitNetworkError' generated error");
    }

    const Status migrationCommitStatus =
        (commitChunkMigrationResponse.isOK() ? commitChunkMigrationResponse.getValue().commandStatus
                                             : commitChunkMigrationResponse.getStatus());

    if (!migrationCommitStatus.isOK()) {
        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _configsvrCommitChunkMigration may have
        // done
        log() << "Error occurred while committing the migration. Performing a majority write "
                 "against the config server to obtain its latest optime"
              << causedBy(redact(migrationCommitStatus));

        Status status = grid.catalogClient(txn)->logChange(
            txn,
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

        fassertStatusOK(
            40137,
            {status.code(),
             str::stream() << "Failed to commit migration for chunk " << _args.toString()
                           << " due to "
                           << redact(migrationCommitStatus)
                           << ". Updating the optime with a write before refreshing the "
                           << "metadata also failed with "
                           << redact(status)});
    }

    // Do a best effort attempt to incrementally refresh the metadata. If this fails, just clear it
    // up so that subsequent requests will try to do a full refresh.
    ChunkVersion unusedShardVersion;
    Status refreshStatus =
        ShardingState::get(txn)->refreshMetadataNow(txn, getNss(), &unusedShardVersion);

    if (refreshStatus.isOK()) {
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetCollection autoColl(txn, getNss(), MODE_IS);

        auto refreshedMetadata = CollectionShardingState::get(txn, getNss())->getMetadata();

        if (!refreshedMetadata) {
            return {ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Chunk move failed because collection '" << getNss().ns()
                                  << "' is no longer sharded. The migration commit error was: "
                                  << migrationCommitStatus.toString()};
        }

        if (refreshedMetadata->keyBelongsToMe(_args.getMinKey())) {
            // The chunk modification was not applied, so report the original error
            return {migrationCommitStatus.code(),
                    str::stream() << "Chunk move was not successful due to "
                                  << migrationCommitStatus.reason()};
        }

        // Migration succeeded
        log() << "Migration succeeded and updated collection version to "
              << refreshedMetadata->getCollVersion();
    } else {
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, getNss(), MODE_IX, MODE_X);

        CollectionShardingState::get(txn, getNss())->refreshMetadata(txn, nullptr);

        log() << "Failed to refresh metadata after a failed commit attempt. Metadata was cleared "
                 "so it will get a full refresh when accessed again"
              << causedBy(redact(refreshStatus));

        // We don't know whether migration succeeded or failed
        return {migrationCommitStatus.code(),
                str::stream() << "Failed to refresh metadata after migration commit due to "
                              << refreshStatus.toString()};
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    scopedGuard.Dismiss();
    _cleanup(txn);

    grid.catalogClient(txn)->logChange(txn,
                                       "moveChunk.commit",
                                       getNss().ns(),
                                       BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey()
                                                  << "from"
                                                  << _args.getFromShardId()
                                                  << "to"
                                                  << _args.getToShardId()),
                                       ShardingCatalogClient::kMajorityWriteConcern);

    return Status::OK();
}

void MigrationSourceManager::cleanupOnError(OperationContext* txn) {
    if (_state == kDone) {
        return;
    }

    grid.catalogClient(txn)->logChange(txn,
                                       "moveChunk.error",
                                       getNss().ns(),
                                       BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey()
                                                  << "from"
                                                  << _args.getFromShardId()
                                                  << "to"
                                                  << _args.getToShardId()),
                                       ShardingCatalogClient::kMajorityWriteConcern);

    _cleanup(txn);
}

void MigrationSourceManager::_cleanup(OperationContext* txn) {
    invariant(_state != kDone);

    auto cloneDriver = [&]() {
        // Unregister from the collection's sharding state
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, getNss().ns());

        // The migration source manager is not visible anymore after it is unregistered from the
        // collection
        css->clearMigrationSourceManager(txn);

        // Leave the critical section.
        if (_critSecSignal) {
            _critSecSignal->set();
        }

        return std::move(_cloneDriver);
    }();

    // Decrement the metadata op counter outside of the collection lock in order to hold it for as
    // short as possible.
    if (_state == kCriticalSection || _state == kCloneCompleted) {
        ShardingStateRecovery::endMetadataOp(txn);
    }

    if (cloneDriver) {
        cloneDriver->cancelClone(txn);
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
