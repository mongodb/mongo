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
MONGO_FP_DECLARE(hangBeforeCommitMigration);
MONGO_FP_DECLARE(hangBeforeLeavingCriticalSection);

MigrationSourceManager::MigrationSourceManager(OperationContext* txn, MoveChunkRequest request)
    : _args(std::move(request)), _startTime() {
    invariant(!txn->lockState()->isLocked());

    // Disallow moving a chunk to ourselves
    uassert(ErrorCodes::InvalidOptions,
            "Destination shard cannot be the same as source",
            _args.getFromShardId() != _args.getToShardId());

    const auto& oss = OperationShardingState::get(txn);
    uassert(ErrorCodes::InvalidOptions, "collection version is missing", oss.hasShardVersion());

    // Even though the moveChunk command transmits a value in the operation's shardVersion field,
    // this value does not actually contain the shard version, but the global collection version.
    const ChunkVersion expectedCollectionVersion = oss.getShardVersion(_args.getNss());

    log() << "Starting chunk migration for "
          << redact(ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString())
          << " with expected collection version " << expectedCollectionVersion;

    // Now that the collection is locked, snapshot the metadata and fetch the latest versions
    ShardingState* const shardingState = ShardingState::get(txn);

    ChunkVersion shardVersion;

    Status refreshStatus =
        shardingState->refreshMetadataNow(txn, _args.getNss().ns(), &shardVersion);
    if (!refreshStatus.isOK()) {
        uasserted(refreshStatus.code(),
                  str::stream() << "cannot start migrate of chunk "
                                << ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString()
                                << " due to "
                                << refreshStatus.toString());
    }

    if (shardVersion.majorVersion() == 0) {
        // If the major version is zero, this means we do not have any chunks locally to migrate in
        // the first place
        uasserted(ErrorCodes::IncompatibleShardingMetadata,
                  str::stream() << "cannot start migrate of chunk "
                                << ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString()
                                << " with zero shard version");
    }

    // Snapshot the committed metadata from the time the migration starts
    {
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

        _committedMetadata = CollectionShardingState::get(txn, _args.getNss())->getMetadata();
        _keyPattern = _committedMetadata->getKeyPattern();
    }

    const ChunkVersion collectionVersion = _committedMetadata->getCollVersion();

    if (expectedCollectionVersion.epoch() != collectionVersion.epoch()) {
        throw SendStaleConfigException(
            _args.getNss().ns(),
            str::stream() << "cannot move chunk "
                          << ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString()
                          << " because collection may have been dropped. "
                          << "current epoch: "
                          << collectionVersion.epoch()
                          << ", cmd epoch: "
                          << expectedCollectionVersion.epoch(),
            expectedCollectionVersion,
            collectionVersion);
    }

    // With nonzero shard version, we must have a coll version >= our shard version
    invariant(collectionVersion >= shardVersion);

    // With nonzero shard version, we must have a shard key
    invariant(!_committedMetadata->getKeyPattern().isEmpty());

    ChunkType origChunk;
    if (!_committedMetadata->getNextChunk(_args.getMinKey(), &origChunk)) {
        // If this assertion is hit, it means that whoever called the shard moveChunk command
        // (mongos or the CSRS balancer) did not check whether the chunk actually belongs to this
        // shard. It is a benign error and does not indicate data corruption.
        uasserted(40145,
                  str::stream() << "Chunk with bounds "
                                << ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString()
                                << " is not owned by this shard.");
    }

    uassert(40146,
            str::stream() << "Unable to find chunk with the exact bounds "
                          << ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString()
                          << " at collection version "
                          << collectionVersion.toString()
                          << ". This indicates corrupted metadata.",
            origChunk.getMin().woCompare(_args.getMinKey()) == 0 &&
                origChunk.getMax().woCompare(_args.getMaxKey()) == 0);
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
                                       _args.getNss().ns(),
                                       BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey()
                                                  << "from"
                                                  << _args.getFromShardId()
                                                  << "to"
                                                  << _args.getToShardId()),
                                       ShardingCatalogClient::kMajorityWriteConcern);

    _cloneDriver = stdx::make_unique<MigrationChunkClonerSourceLegacy>(
        _args, _committedMetadata->getKeyPattern());

    {
        // Register for notifications from the replication subsystem
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());
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
        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());
        if (!css->getMetadata() ||
            !css->getMetadata()->getCollVersion().equals(_committedMetadata->getCollVersion())) {
            return {ErrorCodes::IncompatibleShardingMetadata,
                    str::stream()
                        << "Sharding metadata changed while holding distributed lock. Expected: "
                        << _committedMetadata->getCollVersion().toString()
                        << ", actual: "
                        << css->getMetadata()->getCollVersion().toString()};
        }

        // IMPORTANT: After this line, the critical section is in place and needs to be rolled back
        // if anything fails, which would prevent commit to the config servers.
        _critSecSignal = std::make_shared<Notification<void>>();
    }

    log() << "Successfully entered critical section.";

    _state = kCriticalSection;
    scopedGuard.Dismiss();
    return Status::OK();
}

Status MigrationSourceManager::commitDonateChunk(OperationContext* txn) {
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

    ChunkType migratedChunkType;
    migratedChunkType.setMin(_args.getMinKey());
    migratedChunkType.setMax(_args.getMaxKey());

    // If we have chunks left on the FROM shard, bump the version of one of them as well. This will
    // change the local collection major version, which indicates to other processes that the chunk
    // metadata has changed and they should refresh.
    boost::optional<ChunkType> controlChunkType = boost::none;
    if (_committedMetadata->getNumChunks() > 1) {
        ChunkType differentChunk;
        invariant(_committedMetadata->getDifferentChunk(_args.getMinKey(), &differentChunk));
        invariant(differentChunk.getMin().woCompare(_args.getMinKey()) != 0);
        controlChunkType = std::move(differentChunk);
    } else {
        log() << "moveChunk moved last chunk out for collection '" << _args.getNss().ns() << "'";
    }

    BSONObjBuilder builder;
    CommitChunkMigrationRequest::appendAsCommand(&builder,
                                                 _args.getNss(),
                                                 _args.getFromShardId(),
                                                 _args.getToShardId(),
                                                 migratedChunkType,
                                                 controlChunkType,
                                                 _committedMetadata->getCollVersion(),
                                                 _args.getTakeDistLock());

    builder.append(kWriteConcernField, kMajorityWriteConcern.toBSON());

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeCommitMigration);

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

    if (migrationCommitStatus.isOK()) {
        // Now that _configsvrCommitChunkMigration succeeded and the a collection version is
        // committed, update the collection metadata to the new collection version in the command
        // response and forget the migrated chunk.

        ChunkVersion committedCollVersion;
        if (controlChunkType) {
            committedCollVersion = fassertStatusOK(
                40084,
                ChunkVersion::parseFromBSONWithFieldForCommands(
                    commitChunkMigrationResponse.getValue().response, kControlChunkVersionField));
        } else {
            committedCollVersion = fassertStatusOK(
                40083,
                ChunkVersion::parseFromBSONWithFieldForCommands(
                    commitChunkMigrationResponse.getValue().response, kMigratedChunkVersionField));
        }

        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        ChunkType migratingChunkToForget;
        migratingChunkToForget.setMin(_args.getMinKey());
        migratingChunkToForget.setMax(_args.getMaxKey());

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());
        css->refreshMetadata(
            txn, _committedMetadata->cloneMigrate(migratingChunkToForget, committedCollVersion));
        _committedMetadata = css->getMetadata();
    } else {
        // This could be an unrelated error (e.g. network error). Check whether the metadata update
        // succeeded by refreshing the collection metadata from the config server and checking that
        // the original chunks no longer exist.

        warning() << "Migration metadata commit may have failed: refreshing metadata to check"
                  << causedBy(migrationCommitStatus);

        // Need to get the latest optime in case the refresh request goes to a secondary --
        // otherwise the read won't wait for the write that _configsvrCommitChunkMigration may have
        // done.
        Status status = grid.catalogClient(txn)->logChange(
            txn,
            "moveChunk.validating",
            _args.getNss().ns(),
            BSON("min" << _args.getMinKey() << "max" << _args.getMaxKey() << "from"
                       << _args.getFromShardId()
                       << "to"
                       << _args.getToShardId()),
            ShardingCatalogClient::kMajorityWriteConcern);
        if ((ErrorCodes::isInterruption(status.code()) ||
             ErrorCodes::isShutdownError(status.code())) &&
            inShutdown()) {
            // Since the server is already doing a clean shutdown, this call will just join the
            // previous shutdown call
            shutdown(waitForShutdown());
        }

        fassertStatusOK(40137,
                        {status.code(),
                         str::stream()
                             << "Failed to commit chunk ["
                             << redact(ChunkRange(_args.getMinKey(), _args.getMaxKey()).toString())
                             << " due to "
                             << migrationCommitStatus.toString()
                             << ", and updating the optime with a write before refreshing the "
                             << "metadata also failed with "
                             << status.toString()});

        // Once the write above has succeeded we have an up to date config server optime. In this
        // case do a best effort attempt to incrementally refresh the metadata. If this fails, just
        // clear it up so that subsequent requests will try to do a full refresh.
        ChunkVersion unusedShardVersion;
        Status refreshStatus = ShardingState::get(txn)->refreshMetadataNow(
            txn, _args.getNss().ns(), &unusedShardVersion);

        if (refreshStatus.isOK()) {
            ScopedTransaction scopedXact(txn, MODE_IS);
            AutoGetCollection autoColl(txn, _args.getNss(), MODE_IS);

            auto refreshedMetadata =
                CollectionShardingState::get(txn, _args.getNss())->getMetadata();

            if (refreshedMetadata->keyBelongsToMe(_args.getMinKey())) {
                // The chunk modification was not applied, so report the original error
                return {migrationCommitStatus.code(),
                        str::stream() << "Chunk move was not successful due to "
                                      << migrationCommitStatus.reason()};
            }

            // Migration succeeded
            _committedMetadata = std::move(refreshedMetadata);

            log() << "moveChunk updated collection '" << _args.getNss().ns()
                  << "' to collection version '" << _committedMetadata->getCollVersion() << "'.";
        } else {
            ScopedTransaction scopedXact(txn, MODE_IX);
            AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

            CollectionShardingState::get(txn, _args.getNss())->refreshMetadata(txn, nullptr);

            log() << "moveChunk failed to refresh metadata for collection '" << _args.getNss().ns()
                  << "'. Metadata will be cleared so it gets a full refresh"
                  << causedBy(refreshStatus);

            return {migrationCommitStatus.code(),
                    str::stream() << "Failed to refresh metadata after migration commit due to "
                                  << refreshStatus.reason()};
        }
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLeavingCriticalSection);

    scopedGuard.Dismiss();
    _cleanup(txn);

    grid.catalogClient(txn)->logChange(txn,
                                       "moveChunk.commit",
                                       _args.getNss().ns(),
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
                                       _args.getNss().ns(),
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

    {
        // Unregister from the collection's sharding state
        ScopedTransaction scopedXact(txn, MODE_IX);
        AutoGetCollection autoColl(txn, _args.getNss(), MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(txn, _args.getNss().ns());

        // The migration source manager is not visible anymore after it is unregistered from the
        // collection
        css->clearMigrationSourceManager(txn);

        // Leave the critical section.
        if (_state == kCriticalSection) {
            _critSecSignal->set();
        }
    }

    // Decrement the metadata op counter outside of the collection lock in order to hold it for as
    // short as possible.
    if (_state == kCriticalSection) {
        ShardingStateRecovery::endMetadataOp(txn);
    }

    if (_cloneDriver) {
        _cloneDriver->cancelClone(txn);
        _cloneDriver.reset();
    }

    _state = kDone;
}

BSONObj MigrationSourceManager::getMigrationStatusReport() const {
    return migrationutil::makeMigrationStatusDocument(_args.getNss(),
                                                      _args.getFromShardId(),
                                                      _args.getToShardId(),
                                                      true,
                                                      _args.getMinKey(),
                                                      _args.getMaxKey());
}

}  // namespace mongo
