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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer/scoped_migration_request.h"
#include "mongo/s/balancer/type_migration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/log.h"
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
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

/**
 * Parses the specified asynchronous command response and converts it to status to use as outcome of
 * an asynchronous migration command. It is necessary for two reasons:
 *  - Preserve backwards compatibility with 3.2 and earlier, where the move chunk command instead of
 * returning a ChunkTooBig status includes an extra field in the response.
 *  - Convert CallbackCanceled errors into BalancerInterrupted for the cases where the migration
 * manager is being stopped at replica set stepdown. This return code allows the mongos calling
 * logic to retry the operation on a new primary.
 */
Status extractMigrationStatusFromRemoteCommandResponse(const RemoteCommandResponse& response,
                                                       bool isStopping) {
    if (!response.isOK()) {
        if (response.status == ErrorCodes::CallbackCanceled && isStopping) {
            return {ErrorCodes::BalancerInterrupted,
                    "Migration interrupted because the balancer is stopping"};
        }

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
StatusWith<DistLockHandle> acquireDistLock(OperationContext* txn,
                                           const OID& lockSessionID,
                                           const NamespaceString& nss) {
    const std::string whyMessage(stream() << "Migrating chunk(s) in collection " << nss.ns());

    auto statusWithDistLockHandle =
        Grid::get(txn)->catalogClient(txn)->getDistLockManager()->lockWithSessionID(
            txn, nss.ns(), whyMessage, lockSessionID, DistLockManager::kSingleLockAttemptTimeout);

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

/**
 * Checks whether the Status has an error caused by replSetStepDown or server shut down.
 */
bool isInterruptError(Status status) {
    return (status == ErrorCodes::BalancerInterrupted ||
            ErrorCodes::isInterruption(status.code()) ||
            ErrorCodes::isShutdownError(status.code()));
}

}  // namespace

MigrationManager::MigrationManager(ServiceContext* serviceContext)
    : _serviceContext(serviceContext), _lockSessionID(OID::gen()) {}

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

    MigrationStatuses migrationStatuses;

    vector<MigrateInfo> rescheduledMigrations;

    {
        std::map<MigrationIdentifier, ScopedMigrationRequest> scopedMigrationRequests;
        vector<std::pair<shared_ptr<Notification<Status>>, MigrateInfo>> responses;

        for (const auto& migrateInfo : migrateInfos) {
            // Write a document to the config.migrations collection, in case this migration must be
            // recovered by the Balancer. Fail if the chunk is already moving.
            auto statusWithScopedMigrationRequest =
                ScopedMigrationRequest::writeMigration(txn, migrateInfo);
            if (!statusWithScopedMigrationRequest.isOK()) {
                migrationStatuses.emplace(migrateInfo.getName(),
                                          std::move(statusWithScopedMigrationRequest.getStatus()));
                continue;
            }
            scopedMigrationRequests.emplace(migrateInfo.getName(),
                                            std::move(statusWithScopedMigrationRequest.getValue()));

            responses.emplace_back(_schedule(txn,
                                             migrateInfo,
                                             false,  // Config server takes the collection dist lock
                                             maxChunkSizeBytes,
                                             secondaryThrottle,
                                             waitForDelete),
                                   migrateInfo);
        }

        // Wait for all the scheduled migrations to complete and note the ones, which failed with a
        // LockBusy error code. These need to be executed serially, without the distributed lock
        // being held by the config server for backwards compatibility with 3.2 shards.
        for (auto& response : responses) {
            auto notification = std::move(response.first);
            auto migrateInfo = std::move(response.second);

            Status responseStatus = notification->get();

            if (responseStatus == ErrorCodes::LockBusy) {
                rescheduledMigrations.emplace_back(std::move(migrateInfo));
            } else {
                if (isInterruptError(responseStatus)) {
                    auto it = scopedMigrationRequests.find(migrateInfo.getName());
                    invariant(it != scopedMigrationRequests.end());
                    it->second.keepDocumentOnDestruct();
                }

                migrationStatuses.emplace(migrateInfo.getName(), std::move(responseStatus));
            }
        }
    }

    // Schedule all 3.2 compatibility migrations sequentially
    for (const auto& migrateInfo : rescheduledMigrations) {
        // Write a document to the config.migrations collection, in case this migration must be
        // recovered by the Balancer. Fail if the chunk is already moving.
        auto statusWithScopedMigrationRequest =
            ScopedMigrationRequest::writeMigration(txn, migrateInfo);
        if (!statusWithScopedMigrationRequest.isOK()) {
            migrationStatuses.emplace(migrateInfo.getName(),
                                      std::move(statusWithScopedMigrationRequest.getStatus()));
            continue;
        }

        Status responseStatus = _schedule(txn,
                                          migrateInfo,
                                          true,  // Shard takes the collection dist lock
                                          maxChunkSizeBytes,
                                          secondaryThrottle,
                                          waitForDelete)
                                    ->get();

        if (isInterruptError(responseStatus)) {
            statusWithScopedMigrationRequest.getValue().keepDocumentOnDestruct();
        }

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
    _waitForRecovery();

    // Write a document to the config.migrations collection, in case this migration must be
    // recovered by the Balancer. Fail if the chunk is already moving.
    auto statusWithScopedMigrationRequest =
        ScopedMigrationRequest::writeMigration(txn, migrateInfo);
    if (!statusWithScopedMigrationRequest.isOK()) {
        return statusWithScopedMigrationRequest.getStatus();
    }

    Status status = _schedule(txn,
                              migrateInfo,
                              false,  // Config server takes the collection dist lock
                              maxChunkSizeBytes,
                              secondaryThrottle,
                              waitForDelete)
                        ->get();

    auto scopedCMStatus = ScopedChunkManager::getExisting(txn, NamespaceString(migrateInfo.ns));
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    auto scopedCM = std::move(scopedCMStatus.getValue());
    ChunkManager* const cm = scopedCM.cm();

    // Regardless of the failure mode, if the chunk's current shard matches the destination, deem
    // the move as success.
    auto chunk = cm->findIntersectingChunkWithSimpleCollation(txn, migrateInfo.minKey);
    invariant(chunk);

    if (isInterruptError(status)) {
        statusWithScopedMigrationRequest.getValue().keepDocumentOnDestruct();
        // We want the mongos to get a retriable error, and not make its replica set monitor
        // interpret something like InterruptedDueToReplStateChange as the config server when the
        // error comes from the shard.
        return {ErrorCodes::BalancerInterrupted, status.reason()};
    } else if (chunk->getShardId() == migrateInfo.to) {
        return Status::OK();
    }

    return status;
}

void MigrationManager::startRecoveryAndAcquireDistLocks(OperationContext* txn) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kStopped);
        invariant(_migrationRecoveryMap.empty());
        _state = State::kRecovering;
    }

    auto scopedGuard = MakeGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(txn);
    });

    // Load the active migrations from the config.migrations collection.
    auto statusWithMigrationsQueryResponse =
        Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString(MigrationType::ConfigNS),
            BSONObj(),
            BSONObj(),
            boost::none);

    if (!statusWithMigrationsQueryResponse.isOK()) {
        warning() << "Unable to read config.migrations collection documents for balancer migration"
                  << " recovery. Abandoning balancer recovery."
                  << causedBy(redact(statusWithMigrationsQueryResponse.getStatus()));
        return;
    }

    for (const BSONObj& migration : statusWithMigrationsQueryResponse.getValue().docs) {
        auto statusWithMigrationType = MigrationType::fromBSON(migration);
        if (!statusWithMigrationType.isOK()) {
            // The format of this migration document is incorrect. The balancer holds a distlock for
            // this migration, but without parsing the migration document we cannot identify which
            // distlock must be released. So we must release all distlocks.
            warning() << "Unable to parse config.migrations document '"
                      << redact(migration.toString())
                      << "' for balancer migration recovery. Abandoning balancer recovery."
                      << causedBy(redact(statusWithMigrationType.getStatus()));
            return;
        }
        MigrateInfo migrateInfo = statusWithMigrationType.getValue().toMigrateInfo();

        auto it = _migrationRecoveryMap.find(NamespaceString(migrateInfo.ns));
        if (it == _migrationRecoveryMap.end()) {
            std::list<MigrateInfo> list;
            it = _migrationRecoveryMap.insert(std::make_pair(NamespaceString(migrateInfo.ns), list))
                     .first;

            // Reacquire the matching distributed lock for this namespace.
            const std::string whyMessage(stream() << "Migrating chunk(s) in collection "
                                                  << redact(migrateInfo.ns));
            auto statusWithDistLockHandle =
                Grid::get(txn)
                    ->catalogClient(txn)
                    ->getDistLockManager()
                    ->tryLockWithLocalWriteConcern(txn, migrateInfo.ns, whyMessage, _lockSessionID);
            if (!statusWithDistLockHandle.isOK() &&
                statusWithDistLockHandle.getStatus() != ErrorCodes::LockStateChangeFailed) {
                // LockStateChangeFailed is alright because that should mean a 3.2 shard has it for
                // the active migration.
                warning() << "Failed to acquire distributed lock for collection '"
                          << redact(migrateInfo.ns)
                          << "' during balancer recovery of an active migration. Abandoning"
                          << " balancer recovery."
                          << causedBy(redact(statusWithDistLockHandle.getStatus()));
                return;
            }
        }

        it->second.push_back(std::move(migrateInfo));
    }

    scopedGuard.Dismiss();
}

void MigrationManager::finishRecovery(OperationContext* txn,
                                      uint64_t maxChunkSizeBytes,
                                      const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                      bool waitForDelete) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kStopping) {
            _migrationRecoveryMap.clear();
            return;
        }

        // If recovery was abandoned in startRecovery, then there is no more to do.
        if (_state == State::kEnabled) {
            invariant(_migrationRecoveryMap.empty());
            return;
        }

        invariant(_state == State::kRecovering);
    }

    auto scopedGuard = MakeGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(txn);
    });

    // Schedule recovered migrations.
    vector<ScopedMigrationRequest> scopedMigrationRequests;
    vector<shared_ptr<Notification<Status>>> responses;

    for (auto& nssAndMigrateInfos : _migrationRecoveryMap) {
        auto scopedCMStatus = ScopedChunkManager::getExisting(txn, nssAndMigrateInfos.first);
        if (!scopedCMStatus.isOK()) {
            // This shouldn't happen because the collection was intact and sharded when the previous
            // config primary was active and the dist locks have been held by the balancer
            // throughout. Abort migration recovery.
            warning() << "Unable to reload chunk metadata for collection '"
                      << nssAndMigrateInfos.first
                      << "' during balancer recovery. Abandoning recovery."
                      << causedBy(redact(scopedCMStatus.getStatus()));
            return;
        }

        auto scopedCM = std::move(scopedCMStatus.getValue());
        ChunkManager* const cm = scopedCM.cm();

        auto itMigrateInfo = nssAndMigrateInfos.second.begin();
        invariant(itMigrateInfo != nssAndMigrateInfos.second.end());
        while (itMigrateInfo != nssAndMigrateInfos.second.end()) {
            auto chunk = cm->findIntersectingChunkWithSimpleCollation(txn, itMigrateInfo->minKey);
            invariant(chunk);

            if (chunk->getShardId() != itMigrateInfo->from) {
                // Chunk is no longer on the source shard of the migration. Erase the migration doc
                // and drop the distlock if it is only held for this migration.
                ScopedMigrationRequest::createForRecovery(
                    txn, NamespaceString(itMigrateInfo->ns), itMigrateInfo->minKey);
                if (nssAndMigrateInfos.second.size() == 1) {
                    Grid::get(txn)->catalogClient(txn)->getDistLockManager()->unlock(
                        txn, _lockSessionID, itMigrateInfo->ns);
                }
                itMigrateInfo = nssAndMigrateInfos.second.erase(itMigrateInfo);
            } else {
                scopedMigrationRequests.emplace_back(ScopedMigrationRequest::createForRecovery(
                    txn, NamespaceString(itMigrateInfo->ns), itMigrateInfo->minKey));

                responses.emplace_back(
                    _schedule(txn,
                              *itMigrateInfo,
                              false,  // Config server takes the collection dist lock
                              maxChunkSizeBytes,
                              secondaryThrottle,
                              waitForDelete));

                ++itMigrateInfo;
            }
        }
    }

    _migrationRecoveryMap.clear();
    scopedGuard.Dismiss();

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kRecovering) {
            _state = State::kEnabled;
            _condVar.notify_all();
        }
    }

    // Wait for each migration to finish, as usual.
    for (auto& response : responses) {
        response->get();
    }
}

void MigrationManager::interruptAndDisableMigrations() {
    executor::TaskExecutor* const executor =
        Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state == State::kEnabled || _state == State::kRecovering);
    _state = State::kStopping;

    // Interrupt any active migrations with dist lock
    for (auto& cmsEntry : _activeMigrationsWithDistLock) {
        auto* cms = &cmsEntry.second;

        for (auto& migration : cms->migrations) {
            if (migration.callbackHandle) {
                executor->cancel(*migration.callbackHandle);
            }
        }
    }

    // Interrupt any active migrations without dist lock
    for (auto& migration : _activeMigrationsWithoutDistLock) {
        if (migration.callbackHandle) {
            executor->cancel(*migration.callbackHandle);
        }
    }

    _checkDrained_inlock();
}

void MigrationManager::drainActiveMigrations() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (_state == State::kStopped)
        return;
    invariant(_state == State::kStopping);

    _condVar.wait(lock, [this] {
        return _activeMigrationsWithDistLock.empty() && _activeMigrationsWithoutDistLock.empty();
    });

    _state = State::kStopped;
}

shared_ptr<Notification<Status>> MigrationManager::_schedule(
    OperationContext* txn,
    const MigrateInfo& migrateInfo,
    bool shardTakesCollectionDistLock,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    const NamespaceString nss(migrateInfo.ns);

    // Ensure we are not stopped in order to avoid doing the extra work
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != State::kEnabled && _state != State::kRecovering) {
            return std::make_shared<Notification<Status>>(
                Status(ErrorCodes::BalancerInterrupted,
                       "Migration cannot be executed because the balancer is not running"));
        }
    }


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
    if (SimpleBSONObjComparator::kInstance.evaluate(chunk->getMin() != migrateInfo.minKey) ||
        SimpleBSONObjComparator::kInstance.evaluate(chunk->getMax() != migrateInfo.maxKey)) {
        return std::make_shared<Notification<Status>>(Status(
            ErrorCodes::IncompatibleShardingMetadata,
            stream() << "Chunk " << ChunkRange(migrateInfo.minKey, migrateInfo.maxKey).toString()
                     << " does not exist."));
    }

    const auto fromShardStatus = Grid::get(txn)->shardRegistry()->getShard(txn, migrateInfo.from);
    if (!fromShardStatus.isOK()) {
        return std::make_shared<Notification<Status>>(std::move(fromShardStatus.getStatus()));
    }

    const auto fromShard = fromShardStatus.getValue();
    auto fromHostStatus =
        fromShard->getTargeter()->findHost(txn, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!fromHostStatus.isOK()) {
        return std::make_shared<Notification<Status>>(std::move(fromHostStatus.getStatus()));
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

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (_state != State::kEnabled && _state != State::kRecovering) {
        return std::make_shared<Notification<Status>>(
            Status(ErrorCodes::BalancerInterrupted,
                   "Migration cannot be executed because the balancer is not running"));
    }

    Migration migration(nss, builder.obj());

    auto retVal = migration.completionNotification;

    if (shardTakesCollectionDistLock) {
        _scheduleWithoutDistLock_inlock(txn, fromHostStatus.getValue(), std::move(migration));
    } else {
        _scheduleWithDistLock_inlock(txn, fromHostStatus.getValue(), std::move(migration));
    }

    return retVal;
}

void MigrationManager::_scheduleWithDistLock_inlock(OperationContext* txn,
                                                    const HostAndPort& targetHost,
                                                    Migration migration) {
    executor::TaskExecutor* const executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();

    const NamespaceString nss(migration.nss);

    auto it = _activeMigrationsWithDistLock.find(nss);
    if (it == _activeMigrationsWithDistLock.end()) {
        // Acquire the collection distributed lock (blocking call)
        auto distLockHandleStatus = acquireDistLock(txn, _lockSessionID, nss);
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

                stdx::lock_guard<stdx::mutex> lock(_mutex);
                _completeWithDistLock_inlock(
                    txn.get(),
                    itMigration,
                    extractMigrationStatusFromRemoteCommandResponse(
                        args.response, _state != State::kEnabled && _state != State::kRecovering));
            });

    if (callbackHandleWithStatus.isOK()) {
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

    _completeWithDistLock_inlock(txn, itMigration, std::move(callbackHandleWithStatus.getStatus()));
}

void MigrationManager::_completeWithDistLock_inlock(OperationContext* txn,
                                                    MigrationsList::iterator itMigration,
                                                    Status status) {
    const NamespaceString nss(itMigration->nss);

    // Make sure to signal the notification last, after the distributed lock is freed, so that we
    // don't have the race condition where a subsequently scheduled migration finds the dist lock
    // still acquired.
    auto notificationToSignal = itMigration->completionNotification;

    auto it = _activeMigrationsWithDistLock.find(nss);
    invariant(it != _activeMigrationsWithDistLock.end());

    auto collectionMigrationState = &it->second;
    collectionMigrationState->migrations.erase(itMigration);

    if (collectionMigrationState->migrations.empty()) {
        Grid::get(txn)->catalogClient(txn)->getDistLockManager()->unlock(
            txn, collectionMigrationState->distLockHandle, nss.ns());
        _activeMigrationsWithDistLock.erase(it);
        _checkDrained_inlock();
    }

    notificationToSignal->set(status);
}

void MigrationManager::_scheduleWithoutDistLock_inlock(OperationContext* txn,
                                                       const HostAndPort& targetHost,
                                                       Migration migration) {
    executor::TaskExecutor* const executor = Grid::get(txn)->getExecutorPool()->getFixedExecutor();

    _activeMigrationsWithoutDistLock.push_front(std::move(migration));
    auto itMigration = _activeMigrationsWithoutDistLock.begin();

    const RemoteCommandRequest remoteRequest(
        targetHost, NamespaceString::kAdminDb.toString(), itMigration->moveChunkCmdObj, txn);

    StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
        executor->scheduleRemoteCommand(
            remoteRequest,
            [this, itMigration](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                auto notificationToSignal = itMigration->completionNotification;

                stdx::lock_guard<stdx::mutex> lock(_mutex);

                _activeMigrationsWithoutDistLock.erase(itMigration);
                _checkDrained_inlock();

                notificationToSignal->set(extractMigrationStatusFromRemoteCommandResponse(
                    args.response, _state != State::kEnabled && _state != State::kRecovering));
            });

    if (callbackHandleWithStatus.isOK()) {
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

    auto notificationToSignal = itMigration->completionNotification;

    _activeMigrationsWithoutDistLock.erase(itMigration);
    _checkDrained_inlock();

    notificationToSignal->set(std::move(callbackHandleWithStatus.getStatus()));
}

void MigrationManager::_checkDrained_inlock() {
    if (_state == State::kEnabled || _state == State::kRecovering) {
        return;
    }
    invariant(_state == State::kStopping);

    if (_activeMigrationsWithDistLock.empty() && _activeMigrationsWithoutDistLock.empty()) {
        _condVar.notify_all();
    }
}

void MigrationManager::_waitForRecovery() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condVar.wait(lock, [this] { return _state != State::kRecovering; });
}

void MigrationManager::_abandonActiveMigrationsAndEnableManager(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_state == State::kStopping) {
        // The balancer was interrupted. Let the next balancer recover the state.
        return;
    }
    invariant(_state == State::kRecovering);

    auto catalogClient = Grid::get(txn)->catalogClient(txn);

    // Unlock all balancer distlocks we aren't using anymore.
    auto distLockManager = catalogClient->getDistLockManager();
    distLockManager->unlockAll(txn, distLockManager->getProcessID());

    // Clear the config.migrations collection so that those chunks can be scheduled for migration
    // again.
    catalogClient->removeConfigDocuments(
        txn, MigrationType::ConfigNS, BSONObj(), kMajorityWriteConcern);

    _state = State::kEnabled;
    _condVar.notify_all();
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
