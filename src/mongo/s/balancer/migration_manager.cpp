/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/balancer/migration_manager.h"

#include <memory>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::shared_ptr;
using std::vector;
using str::stream;

namespace {

const char kChunkTooBig[] = "chunkTooBig";

/**
 * Parses the specified asynchronous command response and converts it to status to use as outcome of
 * an asynchronous migration command. In particular it is necessary in order to preserve backwards
 * compatibility with 3.2 and earlier, where the move chunk command instead of returning a
 * ChunkTooBig status includes an extra field in the response.
 */
Status extractMigrationStatusFromRemoteCommandResponse(const RemoteCommandResponse& response) {
    if (!response.isOK()) {
        return response.status;
    }

    Status commandStatus = getStatusFromCommandResult(response.data);

    if (!commandStatus.isOK()) {
        bool chunkTooBig = false;
        bsonExtractBooleanFieldWithDefault(response.data, kChunkTooBig, false, &chunkTooBig);
        if (chunkTooBig) {
            commandStatus = {ErrorCodes::ChunkTooBig, commandStatus.reason()};
        }
    }

    return commandStatus;
}

/**
 * Blocking call to acquire the distributed collection lock for the specified namespace.
 */
StatusWith<DistLockHandle> acquireDistLock(OperationContext* txn, const NamespaceString& nss) {
    const std::string whyMessage(stream() << "Migrating chunk(s) in collection " << nss.ns());

    auto statusWithDistLockHandle =
        Grid::get(txn)->catalogClient(txn)->getDistLockManager()->lockWithSessionID(
            txn, nss.ns(), whyMessage, OID::gen(), DistLockManager::kSingleLockAttemptTimeout);

    if (!statusWithDistLockHandle.isOK()) {
        // If we get LockBusy while trying to acquire the collection distributed lock, this implies
        // that a concurrent collection operation is running either on a 3.2 shard or on mongos.
        // Convert it to ConflictingOperationInProgress to better indicate the error.
        //
        // In addition, the code which re-schedules parallel migrations serially for 3.2 shard
        // compatibility uses the LockBusy code as a hint to do the reschedule.
        const ErrorCodes::Error code = (statusWithDistLockHandle == ErrorCodes::LockBusy
                                            ? ErrorCodes::ConflictingOperationInProgress
                                            : statusWithDistLockHandle.getStatus().code());

        return {code,
                stream() << "Could not acquire collection lock for " << nss.ns()
                         << " to migrate chunks, due to "
                         << statusWithDistLockHandle.getStatus().reason()};
    }

    return std::move(statusWithDistLockHandle.getValue());
}

}  // namespace

MigrationManager::MigrationManager() = default;

MigrationManager::~MigrationManager() {
    // The migration manager must be completely quiesced at destruction time
    invariant(_activeMigrationsWithoutDistLock.empty());
}

MigrationStatuses MigrationManager::executeMigrationsForAutoBalance(
    OperationContext* txn,
    const vector<MigrateInfo>& migrateInfos,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {

    vector<std::pair<shared_ptr<Notification<Status>>, MigrateInfo>> responses;

    for (const auto& migrateInfo : migrateInfos) {
        responses.emplace_back(_schedule(txn,
                                         migrateInfo,
                                         false,  // Config server takes the collection dist lock
                                         maxChunkSizeBytes,
                                         secondaryThrottle,
                                         waitForDelete),
                               migrateInfo);
    }

    MigrationStatuses migrationStatuses;

    vector<MigrateInfo> rescheduledMigrations;

    // Wait for all the scheduled migrations to complete and note the ones, which failed with a
    // LockBusy error code. These need to be executed serially, without the distributed lock being
    // held by the config server for backwards compatibility with 3.2 shards.
    for (auto& response : responses) {
        auto notification = std::move(response.first);
        auto migrateInfo = std::move(response.second);

        Status responseStatus = notification->get();

        if (responseStatus == ErrorCodes::LockBusy) {
            rescheduledMigrations.emplace_back(std::move(migrateInfo));
        } else {
            migrationStatuses.emplace(migrateInfo.getName(), std::move(responseStatus));
        }
    }

    // Schedule all 3.2 compatibility migrations sequentially
    for (const auto& migrateInfo : rescheduledMigrations) {
        Status responseStatus = _schedule(txn,
                                          migrateInfo,
                                          true,  // Shard takes the collection dist lock
                                          maxChunkSizeBytes,
                                          secondaryThrottle,
                                          waitForDelete)
                                    ->get();

        migrationStatuses.emplace(migrateInfo.getName(), std::move(responseStatus));
    }

    invariant(migrationStatuses.size() == migrateInfos.size());

    return migrationStatuses;
}

Status MigrationManager::executeManualMigration(
    OperationContext* txn,
    const MigrateInfo& migrateInfo,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    return _schedule(txn,
                     migrateInfo,
                     false,  // Config server takes the collection dist lock
                     maxChunkSizeBytes,
                     secondaryThrottle,
                     waitForDelete)
        ->get();
}

shared_ptr<Notification<Status>> MigrationManager::_schedule(
    OperationContext* txn,
    const MigrateInfo& migrateInfo,
    bool shardTakesCollectionDistLock,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    const NamespaceString nss(migrateInfo.ns);

    // Sanity checks that the chunk being migrated is actually valid. These will be repeated at the
    // shard as well, but doing them here saves an extra network call, which might otherwise fail.
    auto statusWithScopedChunkManager = ScopedChunkManager::getExisting(txn, nss);
    if (!statusWithScopedChunkManager.isOK()) {
        return std::make_shared<Notification<Status>>(
            std::move(statusWithScopedChunkManager.getStatus()));
    }

    ChunkManager* const chunkManager = statusWithScopedChunkManager.getValue().cm();

    auto chunk = chunkManager->findIntersectingChunkWithSimpleCollation(txn, migrateInfo.minKey);
    invariant(chunk);

    // If the chunk is not found exactly as requested, the caller must have stale data
    if (chunk->getMin() != migrateInfo.minKey || chunk->getMax() != migrateInfo.maxKey) {
        return std::make_shared<Notification<Status>>(Status(
            ErrorCodes::IncompatibleShardingMetadata,
            stream() << "Chunk " << ChunkRange(migrateInfo.minKey, migrateInfo.maxKey).toString()
                     << " does not exist."));
    }

    // If chunk is already on the correct shard, just treat the operation as success
    if (chunk->getShardId() == migrateInfo.to) {
        return std::make_shared<Notification<Status>>(Status::OK());
    }

    const auto recipientShard = Grid::get(txn)->shardRegistry()->getShard(txn, migrateInfo.from);
    auto hostStatus = recipientShard->getTargeter()->findHost(
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        RemoteCommandTargeter::selectFindHostMaxWaitTime(txn));
    if (!hostStatus.isOK()) {
        return std::make_shared<Notification<Status>>(std::move(hostStatus.getStatus()));
    }

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        chunkManager->getVersion(),
        Grid::get(txn)->shardRegistry()->getConfigServerConnectionString(),
        migrateInfo.from,
        migrateInfo.to,
        ChunkRange(migrateInfo.minKey, migrateInfo.maxKey),
        chunk->getLastmod(),
        maxChunkSizeBytes,
        secondaryThrottle,
        waitForDelete,
        shardTakesCollectionDistLock);

    Migration migration(nss, builder.obj());

    auto retVal = migration.completionNotification;

    if (shardTakesCollectionDistLock) {
        _scheduleWithoutDistLock(txn, hostStatus.getValue(), std::move(migration));
    } else {
        _scheduleWithDistLock(txn, hostStatus.getValue(), std::move(migration));
    }

    return retVal;
}

void MigrationManager::_scheduleWithDistLock(OperationContext* txn,
                                             const HostAndPort& targetHost,
                                             Migration migration) {
    const NamespaceString nss(migration.nss);

    executor::TaskExecutor* const executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    auto it = _activeMigrationsWithDistLock.find(nss);
    if (it == _activeMigrationsWithDistLock.end()) {
        // Acquire the collection distributed lock (blocking call)
        auto distLockHandleStatus = acquireDistLock(txn, nss);
        if (!distLockHandleStatus.isOK()) {
            migration.completionNotification->set(distLockHandleStatus.getStatus());
            return;
        }

        it = _activeMigrationsWithDistLock
                 .insert(std::make_pair(
                     nss, CollectionMigrationsState(std::move(distLockHandleStatus.getValue()))))
                 .first;
    }

    auto collectionMigrationState = &it->second;

    // Add ourselves to the list of migrations on this collection
    collectionMigrationState->migrations.push_front(std::move(migration));
    auto itMigration = collectionMigrationState->migrations.begin();

    const RemoteCommandRequest remoteRequest(
        targetHost, NamespaceString::kAdminDb.toString(), itMigration->moveChunkCmdObj, txn);

    StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
        executor->scheduleRemoteCommand(
            remoteRequest,
            [this, collectionMigrationState, itMigration](
                const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                Client::initThread(getThreadName().c_str());
                ON_BLOCK_EXIT([&] { Client::destroy(); });
                auto txn = cc().makeOperationContext();

                _completeWithDistLock(
                    txn.get(),
                    itMigration,
                    extractMigrationStatusFromRemoteCommandResponse(args.response));
            });

    if (callbackHandleWithStatus.isOK()) {
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

    // The completion routine takes its own lock
    lock.unlock();

    _completeWithDistLock(txn, itMigration, std::move(callbackHandleWithStatus.getStatus()));
}

void MigrationManager::_completeWithDistLock(OperationContext* txn,
                                             MigrationsList::iterator itMigration,
                                             Status status) {
    const NamespaceString nss(itMigration->nss);

    // Make sure to signal the notification last, after the distributed lock is freed, so that we
    // don't have the race condition where a subsequently scheduled migration finds the dist lock
    // still acquired.
    auto notificationToSignal = itMigration->completionNotification;

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto it = _activeMigrationsWithDistLock.find(nss);
    invariant(it != _activeMigrationsWithDistLock.end());

    auto collectionMigrationState = &it->second;
    collectionMigrationState->migrations.erase(itMigration);

    if (collectionMigrationState->migrations.empty()) {
        Grid::get(txn)->catalogClient(txn)->getDistLockManager()->unlock(
            txn, collectionMigrationState->distLockHandle);
        _activeMigrationsWithDistLock.erase(it);
    }

    notificationToSignal->set(status);
}

void MigrationManager::_scheduleWithoutDistLock(OperationContext* txn,
                                                const HostAndPort& targetHost,
                                                Migration migration) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _activeMigrationsWithoutDistLock.push_front(std::move(migration));
    auto itMigration = _activeMigrationsWithoutDistLock.begin();

    executor::TaskExecutor* const executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();

    const RemoteCommandRequest remoteRequest(
        targetHost, NamespaceString::kAdminDb.toString(), itMigration->moveChunkCmdObj, txn);

    StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
        executor->scheduleRemoteCommand(
            remoteRequest,
            [this, itMigration](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                itMigration->completionNotification->set(
                    extractMigrationStatusFromRemoteCommandResponse(args.response));
                _activeMigrationsWithoutDistLock.erase(itMigration);
            });

    if (callbackHandleWithStatus.isOK()) {
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

    itMigration->completionNotification->set(std::move(callbackHandleWithStatus.getStatus()));
    _activeMigrationsWithoutDistLock.erase(itMigration);
}

MigrationManager::Migration::Migration(NamespaceString inNss, BSONObj inMoveChunkCmdObj)
    : nss(std::move(inNss)),
      moveChunkCmdObj(std::move(inMoveChunkCmdObj)),
      completionNotification(std::make_shared<Notification<Status>>()) {}

MigrationManager::Migration::~Migration() {
    invariant(completionNotification);
}

MigrationManager::CollectionMigrationsState::CollectionMigrationsState(
    DistLockHandle inDistLockHandle)
    : distLockHandle(std::move(inDistLockHandle)) {}

MigrationManager::CollectionMigrationsState::~CollectionMigrationsState() {
    invariant(migrations.empty());
}

}  // namespace mongo
