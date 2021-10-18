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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/migration_manager.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/balancer/scoped_migration_request.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

const char kChunkTooBig[] = "chunkTooBig";  // TODO: delete in 3.8

/**
 * Parses the 'commandResponse' and converts it to a status to use as the outcome of the command.
 * Preserves backwards compatibility with 3.4 and earlier shards that, rather than use a ChunkTooBig
 * error code, include an extra field in the response.
 *
 * TODO: Delete in 3.8
 */
Status extractMigrationStatusFromCommandResponse(const BSONObj& commandResponse) {
    Status commandStatus = getStatusFromCommandResult(commandResponse);

    if (!commandStatus.isOK()) {
        bool chunkTooBig = false;
        bsonExtractBooleanFieldWithDefault(commandResponse, kChunkTooBig, false, &chunkTooBig)
            .transitional_ignore();
        if (chunkTooBig) {
            commandStatus = {ErrorCodes::ChunkTooBig, commandStatus.reason()};
        }
    }

    return commandStatus;
}

/**
 * Returns whether the specified status is an error caused by stepdown of the primary config node
 * currently running the balancer.
 */
bool isErrorDueToConfigStepdown(Status status, bool isStopping) {
    return ((status == ErrorCodes::CallbackCanceled && isStopping) ||
            status == ErrorCodes::BalancerInterrupted ||
            status == ErrorCodes::InterruptedDueToReplStateChange);
}

}  // namespace

MigrationManager::MigrationManager(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

MigrationManager::~MigrationManager() {
    // The migration manager must be completely quiesced at destruction time
    invariant(_activeMigrations.empty());
}

MigrationStatuses MigrationManager::executeMigrationsForAutoBalance(
    OperationContext* opCtx,
    const std::vector<MigrateInfo>& migrateInfos,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {

    MigrationStatuses migrationStatuses;

    ScopedMigrationRequestsMap scopedMigrationRequests;
    std::vector<std::pair<std::shared_ptr<Notification<RemoteCommandResponse>>, MigrateInfo>>
        responses;

    for (const auto& migrateInfo : migrateInfos) {
        responses.emplace_back(_schedule(opCtx,
                                         migrateInfo,
                                         maxChunkSizeBytes,
                                         secondaryThrottle,
                                         waitForDelete,
                                         &scopedMigrationRequests),
                               migrateInfo);
    }

    // Wait for all the scheduled migrations to complete.
    for (auto& response : responses) {
        auto notification = std::move(response.first);
        auto migrateInfo = std::move(response.second);

        const auto& remoteCommandResponse = notification->get();
        const auto migrationInfoName = migrateInfo.getName();

        auto it = scopedMigrationRequests.find(migrationInfoName);
        if (it == scopedMigrationRequests.end()) {
            invariant(!remoteCommandResponse.status.isOK());
            migrationStatuses.emplace(migrationInfoName, std::move(remoteCommandResponse.status));
            continue;
        }

        auto statusWithScopedMigrationRequest = std::move(it->second);

        if (!statusWithScopedMigrationRequest.isOK()) {
            invariant(!remoteCommandResponse.status.isOK());
            migrationStatuses.emplace(migrationInfoName,
                                      std::move(statusWithScopedMigrationRequest.getStatus()));
            continue;
        }

        Status commandStatus = _processRemoteCommandResponse(
            remoteCommandResponse, &statusWithScopedMigrationRequest.getValue());
        migrationStatuses.emplace(migrationInfoName, std::move(commandStatus));
    }

    invariant(migrationStatuses.size() == migrateInfos.size());

    return migrationStatuses;
}

Status MigrationManager::executeManualMigration(
    OperationContext* opCtx,
    const MigrateInfo& migrateInfo,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {

    _waitForRecovery();

    ScopedMigrationRequestsMap scopedMigrationRequests;

    RemoteCommandResponse remoteCommandResponse = _schedule(opCtx,
                                                            migrateInfo,
                                                            maxChunkSizeBytes,
                                                            secondaryThrottle,
                                                            waitForDelete,
                                                            &scopedMigrationRequests)
                                                      ->get();

    auto swCM = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
        opCtx, migrateInfo.nss);
    if (!swCM.isOK()) {
        return swCM.getStatus();
    }

    const auto& cm = swCM.getValue();

    const auto chunk = cm.findIntersectingChunkWithSimpleCollation(migrateInfo.minKey);

    Status commandStatus = remoteCommandResponse.status;

    const auto migrationInfoName = migrateInfo.getName();

    auto it = scopedMigrationRequests.find(migrationInfoName);

    if (it != scopedMigrationRequests.end()) {
        invariant(scopedMigrationRequests.size() == 1);
        auto statusWithScopedMigrationRequest = &it->second;
        commandStatus = _processRemoteCommandResponse(
            remoteCommandResponse, &statusWithScopedMigrationRequest->getValue());
    }

    // Migration calls can be interrupted after the metadata is committed but before the command
    // finishes the waitForDelete stage. Any failovers, therefore, must always cause the moveChunk
    // command to be retried so as to assure that the waitForDelete promise of a successful command
    // has been fulfilled.
    if (chunk.getShardId() == migrateInfo.to && commandStatus != ErrorCodes::BalancerInterrupted) {
        return Status::OK();
    }

    return commandStatus;
}

void MigrationManager::startRecoveryAndAcquireDistLocks(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        invariant(_state == State::kStopped);
        invariant(_migrationRecoveryMap.empty());
        _state = State::kRecovering;
    }

    ScopeGuard scopedGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(opCtx);
    });

    // Load the active migrations from the config.migrations collection.
    auto statusWithMigrationsQueryResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            MigrationType::ConfigNS,
            BSONObj(),
            BSONObj(),
            boost::none);

    if (!statusWithMigrationsQueryResponse.isOK()) {
        LOGV2(21896,
              "Unable to read config.migrations collection documents for balancer migration "
              "recovery. Abandoning balancer recovery: {error}",
              "Unable to read config.migrations documents for balancer migration recovery",
              "error"_attr = redact(statusWithMigrationsQueryResponse.getStatus()));
        return;
    }

    for (const BSONObj& migration : statusWithMigrationsQueryResponse.getValue().docs) {
        auto statusWithMigrationType = MigrationType::fromBSON(migration);
        if (!statusWithMigrationType.isOK()) {
            // The format of this migration document is incorrect. The balancer holds a distlock for
            // this migration, but without parsing the migration document we cannot identify which
            // distlock must be released. So we must release all distlocks.
            LOGV2(21897,
                  "Unable to parse config.migrations document '{migration}' for balancer"
                  "migration recovery. Abandoning balancer recovery: {error}",
                  "Unable to parse config.migrations document for balancer migration recovery",
                  "migration"_attr = redact(migration.toString()),
                  "error"_attr = redact(statusWithMigrationType.getStatus()));
            return;
        }
        MigrationType migrateType = std::move(statusWithMigrationType.getValue());

        auto it = _migrationRecoveryMap.find(NamespaceString(migrateType.getNss()));
        if (it == _migrationRecoveryMap.end()) {
            std::list<MigrationType> list;
            it = _migrationRecoveryMap.insert(std::make_pair(migrateType.getNss(), list)).first;

            // Reacquire the matching distributed lock for this namespace.
            const std::string whyMessage(str::stream() << "Migrating chunk(s) in collection "
                                                       << migrateType.getNss().ns());

            auto status = DistLockManager::get(opCtx)->tryLockDirectWithLocalWriteConcern(
                opCtx, migrateType.getNss().ns(), whyMessage);
            if (!status.isOK()) {
                LOGV2(21898,
                      "Failed to acquire distributed lock for collection {namespace} "
                      "during balancer recovery of an active migration. Abandoning balancer "
                      "recovery: {error}",
                      "Failed to acquire distributed lock for collection "
                      "during balancer recovery of an active migration",
                      "namespace"_attr = migrateType.getNss().ns(),
                      "error"_attr = redact(status));
                return;
            }
        }

        it->second.push_back(std::move(migrateType));
    }

    scopedGuard.dismiss();
}

void MigrationManager::finishRecovery(OperationContext* opCtx,
                                      uint64_t maxChunkSizeBytes,
                                      const MigrationSecondaryThrottleOptions& secondaryThrottle) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
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

    ScopeGuard scopedGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(opCtx);
    });

    // Schedule recovered migrations.
    std::vector<ScopedMigrationRequest> scopedMigrationRequests;
    std::vector<std::shared_ptr<Notification<RemoteCommandResponse>>> responses;

    for (auto& nssAndMigrateInfos : _migrationRecoveryMap) {
        auto& nss = nssAndMigrateInfos.first;
        auto& migrateInfos = nssAndMigrateInfos.second;
        invariant(!migrateInfos.empty());

        auto swCM = Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
            opCtx, nss);
        if (!swCM.isOK()) {
            // This shouldn't happen because the collection was intact and sharded when the previous
            // config primary was active and the dist locks have been held by the balancer
            // throughout. Abort migration recovery.
            LOGV2(21899,
                  "Unable to reload chunk metadata for collection {namespace} during balancer "
                  "recovery. Abandoning recovery: {error}",
                  "Unable to reload chunk metadata for collection during balancer recovery",
                  "namespace"_attr = nss,
                  "error"_attr = redact(swCM.getStatus()));
            return;
        }

        const auto& cm = swCM.getValue();
        const auto uuid = cm.getUUID();
        if (!uuid) {
            // The collection has been dropped, so there is no need to recover the migration.
            continue;
        }

        int scheduledMigrations = 0;

        while (!migrateInfos.empty()) {
            auto migrationType = std::move(migrateInfos.front());
            const auto migrationInfo = migrationType.toMigrateInfo(*uuid);
            auto waitForDelete = migrationType.getWaitForDelete();
            migrateInfos.pop_front();

            try {
                const auto chunk =
                    cm.findIntersectingChunkWithSimpleCollation(migrationInfo.minKey);

                if (chunk.getShardId() != migrationInfo.from) {
                    // Chunk is no longer on the source shard specified by this migration. Erase the
                    // migration recovery document associated with it.
                    ScopedMigrationRequest::createForRecovery(opCtx, nss, migrationInfo.minKey);
                    continue;
                }
            } catch (const ExceptionFor<ErrorCodes::ShardKeyNotFound>&) {
                // The shard key for the collection has changed.
                // Abandon this migration and remove the document associated with it.
                ScopedMigrationRequest::createForRecovery(opCtx, nss, migrationInfo.minKey);
                continue;
            }

            scopedMigrationRequests.emplace_back(
                ScopedMigrationRequest::createForRecovery(opCtx, nss, migrationInfo.minKey));

            scheduledMigrations++;

            responses.emplace_back(_schedule(opCtx,
                                             migrationInfo,
                                             maxChunkSizeBytes,
                                             secondaryThrottle,
                                             waitForDelete,
                                             nullptr));
        }

        // If no migrations were scheduled for this namespace, free the dist lock
        if (!scheduledMigrations) {
            DistLockManager::get(opCtx)->unlock(opCtx, nss.ns());
        }
    }

    _migrationRecoveryMap.clear();
    scopedGuard.dismiss();

    {
        stdx::lock_guard<Latch> lock(_mutex);
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
    auto executor = Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();

    stdx::lock_guard<Latch> lock(_mutex);
    invariant(_state == State::kEnabled || _state == State::kRecovering);
    _state = State::kStopping;

    // Interrupt any active migrations with dist lock
    for (auto& cmsEntry : _activeMigrations) {
        auto& migrations = cmsEntry.second.migrationsList;

        for (auto& migration : migrations) {
            if (migration.callbackHandle) {
                executor->cancel(*migration.callbackHandle);
            }
        }
    }

    _checkDrained(lock);
}

void MigrationManager::drainActiveMigrations() {
    stdx::unique_lock<Latch> lock(_mutex);

    if (_state == State::kStopped)
        return;
    invariant(_state == State::kStopping);
    _condVar.wait(lock, [this] { return _activeMigrations.empty(); });
    _state = State::kStopped;
}

std::shared_ptr<Notification<RemoteCommandResponse>> MigrationManager::_schedule(
    OperationContext* opCtx,
    const MigrateInfo& migrateInfo,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete,
    ScopedMigrationRequestsMap* scopedMigrationRequests) {

    // Ensure we are not stopped in order to avoid doing the extra work
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_state != State::kEnabled && _state != State::kRecovering) {
            return std::make_shared<Notification<RemoteCommandResponse>>(
                Status(ErrorCodes::BalancerInterrupted,
                       "Migration cannot be executed because the balancer is not running"));
        }
    }

    const auto fromShardStatus =
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, migrateInfo.from);
    if (!fromShardStatus.isOK()) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            std::move(fromShardStatus.getStatus()));
    }

    const auto fromShard = fromShardStatus.getValue();
    auto fromHostStatus = fromShard->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!fromHostStatus.isOK()) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            std::move(fromHostStatus.getStatus()));
    }

    BSONObjBuilder builder;
    MoveChunkRequest::appendAsCommand(&builder,
                                      migrateInfo.nss,
                                      migrateInfo.version,
                                      migrateInfo.from,
                                      migrateInfo.to,
                                      ChunkRange(migrateInfo.minKey, migrateInfo.maxKey),
                                      maxChunkSizeBytes,
                                      secondaryThrottle,
                                      waitForDelete,
                                      migrateInfo.forceJumbo);

    stdx::lock_guard<Latch> lock(_mutex);

    if (_state != State::kEnabled && _state != State::kRecovering) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            Status(ErrorCodes::BalancerInterrupted,
                   "Migration cannot be executed because the balancer is not running"));
    }

    Migration migration(migrateInfo.nss, builder.obj());

    auto retVal = migration.completionNotification;

    _acquireDistLockAndSchedule(lock,
                                opCtx,
                                fromHostStatus.getValue(),
                                std::move(migration),
                                migrateInfo,
                                waitForDelete,
                                scopedMigrationRequests);

    return retVal;
}

void MigrationManager::_acquireDistLockAndSchedule(
    WithLock lock,
    OperationContext* opCtx,
    const HostAndPort& targetHost,
    Migration migration,
    const MigrateInfo& migrateInfo,
    bool waitForDelete,
    ScopedMigrationRequestsMap* scopedMigrationRequests) noexcept {
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    const NamespaceString nss(migration.nss);

    auto it = _activeMigrations.find(nss);
    if (it == _activeMigrations.end()) {
        boost::optional<DistLockManager::ScopedLock> scopedLock;
        try {
            scopedLock.emplace(DistLockManager::get(opCtx)->lockDirectLocally(
                opCtx, nss.ns(), DistLockManager::kSingleLockAttemptTimeout));

            const std::string whyMessage(str::stream()
                                         << "Migrating chunk(s) in collection " << nss.ns());
            uassertStatusOK(DistLockManager::get(opCtx)->lockDirect(
                opCtx, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout));
        } catch (const DBException& ex) {
            migration.completionNotification->set(
                ex.toStatus(str::stream() << "Could not acquire collection lock for " << nss.ns()
                                          << " to migrate chunks"));
            return;
        }

        MigrationsState migrationsState(std::move(*scopedLock));
        it = _activeMigrations.insert(std::make_pair(nss, std::move(migrationsState))).first;
    }

    auto migrationRequestStatus = Status::OK();

    if (scopedMigrationRequests) {
        // Write a document to the config.migrations collection, in case this migration must be
        // recovered by the Balancer.
        auto statusWithScopedMigrationRequest =
            ScopedMigrationRequest::writeMigration(opCtx, migrateInfo, waitForDelete);
        if (!statusWithScopedMigrationRequest.isOK()) {
            migrationRequestStatus = std::move(statusWithScopedMigrationRequest.getStatus());
        } else {
            scopedMigrationRequests->emplace(migrateInfo.getName(),
                                             std::move(statusWithScopedMigrationRequest));
        }
    }

    auto migrations = &it->second.migrationsList;

    // Add ourselves to the list of migrations on this collection. From that point onwards, requests
    // must call _complete regardless of success or failure in order to remove it from the list.
    migrations->push_front(std::move(migration));
    auto itMigration = migrations->begin();

    if (!migrationRequestStatus.isOK()) {
        _complete(lock, opCtx, itMigration, std::move(migrationRequestStatus));
        return;
    }

    const RemoteCommandRequest remoteRequest(
        targetHost, NamespaceString::kAdminDb.toString(), itMigration->moveChunkCmdObj, opCtx);

    StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
        executor->scheduleRemoteCommand(
            remoteRequest,
            [this, service = opCtx->getServiceContext(), itMigration](
                const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                ThreadClient tc(getThreadName(), service);
                auto opCtx = cc().makeOperationContext();

                stdx::lock_guard<Latch> lock(_mutex);
                _complete(lock, opCtx.get(), itMigration, args.response);
            });

    if (callbackHandleWithStatus.isOK()) {
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

    _complete(lock, opCtx, itMigration, std::move(callbackHandleWithStatus.getStatus()));
}

void MigrationManager::_complete(WithLock lock,
                                 OperationContext* opCtx,
                                 MigrationsList::iterator itMigration,
                                 const RemoteCommandResponse& remoteCommandResponse) {
    const NamespaceString nss(itMigration->nss);

    // Make sure to signal the notification last, after the distributed lock is freed, so that we
    // don't have the race condition where a subsequently scheduled migration finds the dist lock
    // still acquired.
    auto notificationToSignal = itMigration->completionNotification;

    auto it = _activeMigrations.find(nss);
    invariant(it != _activeMigrations.end());

    auto migrations = &it->second.migrationsList;
    migrations->erase(itMigration);

    if (migrations->empty()) {
        DistLockManager::get(opCtx)->unlock(opCtx, nss.ns());
        _activeMigrations.erase(it);
        _checkDrained(lock);
    }

    notificationToSignal->set(remoteCommandResponse);
}

void MigrationManager::_checkDrained(WithLock) {
    if (_state == State::kEnabled || _state == State::kRecovering) {
        return;
    }
    invariant(_state == State::kStopping);

    if (_activeMigrations.empty()) {
        _condVar.notify_all();
    }
}

void MigrationManager::_waitForRecovery() {
    stdx::unique_lock<Latch> lock(_mutex);
    _condVar.wait(lock, [this] { return _state != State::kRecovering; });
}

void MigrationManager::_abandonActiveMigrationsAndEnableManager(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lock(_mutex);
    if (_state == State::kStopping) {
        // The balancer was interrupted. Let the next balancer recover the state.
        return;
    }
    invariant(_state == State::kRecovering);

    // Unlock all balancer distlocks we aren't using anymore.
    DistLockManager::get(opCtx)->unlockAll(opCtx);

    // Clear the config.migrations collection so that those chunks can be scheduled for migration
    // again.
    Grid::get(opCtx)
        ->catalogClient()
        ->removeConfigDocuments(
            opCtx, MigrationType::ConfigNS, BSONObj(), ShardingCatalogClient::kLocalWriteConcern)
        .transitional_ignore();

    _state = State::kEnabled;
    _condVar.notify_all();
}

Status MigrationManager::_processRemoteCommandResponse(
    const RemoteCommandResponse& remoteCommandResponse,
    ScopedMigrationRequest* scopedMigrationRequest) {

    stdx::lock_guard<Latch> lock(_mutex);
    Status commandStatus(ErrorCodes::InternalError, "Uninitialized value.");

    // Check for local errors sending the remote command caused by stepdown.
    if (isErrorDueToConfigStepdown(remoteCommandResponse.status,
                                   _state != State::kEnabled && _state != State::kRecovering)) {
        scopedMigrationRequest->keepDocumentOnDestruct();
        return {ErrorCodes::BalancerInterrupted,
                str::stream() << "Migration interrupted because the balancer is stopping."
                              << " Command status: " << remoteCommandResponse.status.toString()};
    }

    if (!remoteCommandResponse.isOK()) {
        commandStatus = remoteCommandResponse.status;
    } else {
        // TODO: delete in 3.8
        commandStatus = extractMigrationStatusFromCommandResponse(remoteCommandResponse.data);
    }

    if (!Shard::shouldErrorBePropagated(commandStatus.code())) {
        commandStatus = {ErrorCodes::OperationFailed,
                         str::stream() << "moveChunk command failed on source shard."
                                       << causedBy(commandStatus)};
    }

    // Any failure to remove the migration document should be because the config server is
    // stepping/shutting down. In this case we must fail the moveChunk command with a retryable
    // error so that the caller does not move on to other distlock requiring operations that could
    // fail when the balancer recovers and takes distlocks for migration recovery.
    Status status = scopedMigrationRequest->tryToRemoveMigration();
    if (!status.isOK()) {
        commandStatus = {
            ErrorCodes::BalancerInterrupted,
            str::stream() << "Migration interrupted because the balancer is stopping"
                          << " and failed to remove the config.migrations document."
                          << " Command status: "
                          << (commandStatus.isOK() ? status.toString() : commandStatus.toString())};
    }

    return commandStatus;
}

MigrationManager::Migration::Migration(NamespaceString inNss, BSONObj inMoveChunkCmdObj)
    : nss(std::move(inNss)),
      moveChunkCmdObj(std::move(inMoveChunkCmdObj)),
      completionNotification(std::make_shared<Notification<RemoteCommandResponse>>()) {}

MigrationManager::Migration::~Migration() {
    invariant(completionNotification);
}

MigrationManager::MigrationsState::MigrationsState(DistLockManager::ScopedLock lock)
    : lock(std::move(lock)) {}

}  // namespace mongo
