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

#include <memory>

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/migration_manager.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/represent_as.h"

namespace mongo {

namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

}  // namespace

MigrationManager::Migration::Migration(
    const MigrateInfo& migrateInfo,
    boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle)
    : chunkInfo(&migrateInfo),
      moveChunkCallbackHandle(std::move(callbackHandle)),
      oldShard(false) {}

void MigrationManager::Migration::setCallbackHandle(
    executor::TaskExecutor::CallbackHandle callbackHandle) {
    invariant(!moveChunkCallbackHandle);
    moveChunkCallbackHandle = std::move(callbackHandle);
}

void MigrationManager::Migration::clearCallbackHandle() {
    moveChunkCallbackHandle = boost::none;
}

MigrationManager::DistLockTracker::DistLockTracker(
    boost::optional<DistLockManager::ScopedDistLock> distlock)
    : distributedLock(std::move(distlock)) {
    if (distlock) {
        migrationCounter = 1;
    } else {
        migrationCounter = 0;
    }
}

MigrationStatuses MigrationManager::scheduleMigrations(
    OperationContext* txn,
    const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateMigrations) {
    invariant(_activeMigrations.empty());

    MigrationStatuses migrationStatuses;

    for (auto& migrateInfo : candidateMigrations) {
        _activeMigrations.push_back(Migration(migrateInfo, boost::none));
    }

    _executeMigrations(txn, &migrationStatuses);

    return migrationStatuses;
}

void MigrationManager::_executeMigrations(OperationContext* txn,
                                          MigrationStatuses* migrationStatuses) {
    auto balancerConfig = Grid::get(txn)->getBalancerConfiguration();
    for (auto& migration : _activeMigrations) {
        const NamespaceString nss(migration.chunkInfo->ns);

        auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nss);
        if (!scopedCMStatus.isOK()) {
            // Unable to find the ChunkManager for "nss" for whatever reason; abandon this
            // migration and proceed to the next.
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            migrationStatuses->insert(MigrationStatuses::value_type(
                migration.chunkInfo->getName(), std::move(scopedCMStatus.getStatus())));
            continue;
        }

        ChunkManager* const chunkManager = scopedCMStatus.getValue().cm();
        auto chunk = chunkManager->findIntersectingChunk(txn, migration.chunkInfo->minKey);

        {
            // No need to lock the mutex. Only this function and _takeDistLockForAMigration
            // manipulate "_distributedLocks". No need to protect serial actions.
            if (!_takeDistLockForAMigration(txn, migration, migrationStatuses)) {
                // If there is a lock conflict between the balancer and the shard, or a shard and a
                // shard, the migration has been rescheduled. Otherwise an attempt to take the lock
                // failed for whatever reason and this migration is being abandoned.
                continue;
            }
        }

        BSONObjBuilder builder;
        MoveChunkRequest::appendAsCommand(
            &builder,
            nss,
            chunkManager->getVersion(),
            Grid::get(txn)->shardRegistry()->getConfigServerConnectionString(),
            migration.chunkInfo->from,
            migration.chunkInfo->to,
            ChunkRange(chunk->getMin(), chunk->getMax()),
            balancerConfig->getMaxChunkSizeBytes(),
            balancerConfig->getSecondaryThrottle(),
            balancerConfig->waitForDelete(),
            migration.oldShard ? true : false);  // takeDistLock flag.

        BSONObj moveChunkRequestObj = builder.obj();

        const auto recipientShard = grid.shardRegistry()->getShard(txn, migration.chunkInfo->from);
        const auto host = recipientShard->getTargeter()->findHost(
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            RemoteCommandTargeter::selectFindHostMaxWaitTime(txn));
        if (!host.isOK()) {
            // Unable to find a target shard for whatever reason; abandon this migration and proceed
            // to the next.
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            migrationStatuses->insert(MigrationStatuses::value_type(migration.chunkInfo->getName(),
                                                                    std::move(host.getStatus())));
            continue;
        }

        RemoteCommandRequest remoteRequest(host.getValue(), "admin", moveChunkRequestObj);

        StatusWith<RemoteCommandResponse> remoteCommandResponse(
            Status{ErrorCodes::InternalError, "Uninitialized value"});

        executor::TaskExecutor* executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();

        StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
            executor->scheduleRemoteCommand(remoteRequest,
                                            stdx::bind(&MigrationManager::_checkMigrationCallback,
                                                       this,
                                                       stdx::placeholders::_1,
                                                       txn,
                                                       &migration,
                                                       migrationStatuses));

        if (!callbackHandleWithStatus.isOK()) {
            // Scheduling the migration moveChunk failed.
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            migrationStatuses->insert(MigrationStatuses::value_type(
                migration.chunkInfo->getName(), std::move(callbackHandleWithStatus.getStatus())));
            continue;
        }

        // The moveChunk command was successfully scheduled. Store the callback handle so that the
        // command's return can be waited for later.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        migration.setCallbackHandle(std::move(callbackHandleWithStatus.getValue()));
    }

    _waitForMigrations(txn);
    // At this point, there are no parallel running threads so it is safe not to lock the mutex.

    // All the migrations have returned, release all of the distributed locks that are no longer
    // being used.
    _distributedLocks.clear();

    // If there are rescheduled migrations, move them to active and run the function again.
    if (!_rescheduledMigrations.empty()) {
        // Clear all the callback handles of the rescheduled migrations.
        for (auto migration : _rescheduledMigrations) {
            migration.clearCallbackHandle();
        }
        _activeMigrations = std::move(_rescheduledMigrations);
        _rescheduledMigrations.clear();
        _executeMigrations(txn, migrationStatuses);
    } else {
        _activeMigrations.clear();
    }
}

void MigrationManager::_checkMigrationCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& callbackArgs,
    OperationContext* txn,
    Migration* migration,
    MigrationStatuses* migrationStatuses) {
    const auto& remoteCommandResponseWithStatus = callbackArgs.response;

    if (!remoteCommandResponseWithStatus.isOK()) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        migrationStatuses->insert(
            MigrationStatuses::value_type(migration->chunkInfo->getName(),
                                          std::move(remoteCommandResponseWithStatus.getStatus())));
        return;
    }

    Status commandStatus =
        getStatusFromCommandResult(remoteCommandResponseWithStatus.getValue().data);
    if (commandStatus == ErrorCodes::LockBusy && !migration->oldShard) {
        migration->oldShard = true;
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _rescheduleMigration(*migration);
        return;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    migrationStatuses->insert(
        MigrationStatuses::value_type(migration->chunkInfo->getName(), std::move(commandStatus)));
}

void MigrationManager::_waitForMigrations(OperationContext* txn) const {
    executor::TaskExecutor* executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();
    for (const auto& migration : _activeMigrations) {
        // Block until the command is carried out.
        if (migration.moveChunkCallbackHandle) {
            executor->wait(migration.moveChunkCallbackHandle.get());
        }
    }
}

void MigrationManager::_rescheduleMigration(const Migration& migration) {
    _rescheduledMigrations.push_back(migration);
}

bool MigrationManager::_takeDistLockForAMigration(OperationContext* txn,
                                                  const Migration& migration,
                                                  MigrationStatuses* migrationStatuses) {
    auto it = _distributedLocks.find(migration.chunkInfo->ns);

    if (it == _distributedLocks.end()) {
        // Neither the balancer nor the shard has the distributed collection lock for "ns".
        if (migration.oldShard) {
            DistLockTracker distLockTracker(boost::none);
            _distributedLocks.insert(std::map<std::string, DistLockTracker>::value_type(
                migration.chunkInfo->ns, std::move(distLockTracker)));
        } else {
            auto distlock = _getDistLock(txn, migration);
            if (!distlock.isOK()) {
                // Abandon the migration so the balancer doesn't reschedule endlessly if whatever is
                // preventing the distlock from being acquired doesn't go away.
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                migrationStatuses->insert(MigrationStatuses::value_type(
                    migration.chunkInfo->getName(), std::move(distlock.getStatus())));
                return false;
            }
            DistLockTracker distLockTracker(std::move(distlock.getValue()));
            _distributedLocks.insert(std::map<std::string, DistLockTracker>::value_type(
                migration.chunkInfo->ns, std::move(distLockTracker)));
        }
    } else {
        DistLockTracker* distLockTracker = &(it->second);
        if (!distLockTracker->distributedLock) {
            // Lock conflict. A shard holds the lock for a different migration.
            invariant(distLockTracker->migrationCounter == 0 && !distLockTracker->distributedLock);
            _rescheduleMigration(migration);
            return false;
        } else {
            invariant(distLockTracker->distributedLock && distLockTracker->migrationCounter > 0);
            if (migration.oldShard) {
                // Lock conflict. The balancer holds the lock, so the shard cannot take it yet.
                _rescheduleMigration(migration);
                return false;
            } else {
                ++(distLockTracker->migrationCounter);
            }
        }
    }

    return true;
}

StatusWith<DistLockManager::ScopedDistLock> MigrationManager::_getDistLock(
    OperationContext* txn, const Migration& migration) {
    const std::string whyMessage(
        str::stream()
        << "migrating chunk "
        << ChunkRange(migration.chunkInfo->minKey, migration.chunkInfo->maxKey).toString()
        << " in "
        << migration.chunkInfo->ns);

    StatusWith<DistLockManager::ScopedDistLock> distLockStatus =
        Grid::get(txn)->catalogClient(txn)->distLock(txn, migration.chunkInfo->ns, whyMessage);

    if (!distLockStatus.isOK()) {
        const std::string msg = str::stream()
            << "Could not acquire collection lock for " << migration.chunkInfo->ns
            << " to migrate chunk "
            << ChunkRange(migration.chunkInfo->minKey, migration.chunkInfo->maxKey).toString()
            << " due to " << distLockStatus.getStatus().toString();
        warning() << msg;
        return {distLockStatus.getStatus().code(), msg};
    }

    return std::move(distLockStatus.getValue());
}

}  // namespace mongo
