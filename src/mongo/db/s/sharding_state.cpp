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

#include "mongo/db/s/sharding_state.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace mongo {

using std::shared_ptr;
using std::string;
using std::vector;

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

namespace {

const auto getShardingState = ServiceContext::declareDecoration<ShardingState>();

// Max number of concurrent config server refresh threads
const int kMaxConfigServerRefreshThreads = 3;

// Maximum number of times to try to refresh the collection metadata if conflicts are occurring
const int kMaxNumMetadataRefreshAttempts = 3;

/**
 * Updates the config server field of the shardIdentity document with the given connection string
 * if setName is equal to the config server replica set name.
 *
 * Note: This is intended to be used on a new thread that hasn't called Client::initThread.
 * One example use case is for the ReplicaSetMonitor asynchronous callback when it detects changes
 * to replica set membership.
 */
void updateShardIdentityConfigStringCB(const string& setName, const string& newConnectionString) {
    auto configsvrConnStr = grid.shardRegistry()->getConfigServerConnectionString();
    if (configsvrConnStr.getSetName() != setName) {
        // Ignore all change notification for other sets that are not the config server.
        return;
    }

    Client::initThread("updateShardIdentityConfigConnString");
    auto uniqOpCtx = getGlobalServiceContext()->makeOperationContext(&cc());

    auto status = ShardingState::get(uniqOpCtx.get())
                      ->updateShardIdentityConfigString(uniqOpCtx.get(), newConnectionString);
    if (!status.isOK() && !ErrorCodes::isNotMasterError(status.code())) {
        warning() << "error encountered while trying to update config connection string to "
                  << newConnectionString << redact(status);
    }
}

}  // namespace

const std::set<std::string> ShardingState::_commandsThatInitializeShardingAwareness{
    "_recvChunkStart", "mergeChunks", "moveChunk", "setShardVersion", "splitChunk"};

ShardingState::ShardingState()
    : _initializationState(static_cast<uint32_t>(InitializationState::kNew)),
      _initializationStatus(Status(ErrorCodes::InternalError, "Uninitialized value")),
      _configServerTickets(kMaxConfigServerRefreshThreads),
      _globalInit(&initializeGlobalShardingStateForMongod),
      _scheduleWorkFn([](NamespaceString nss) {}) {}

ShardingState::~ShardingState() = default;

ShardingState* ShardingState::get(ServiceContext* serviceContext) {
    return &getShardingState(serviceContext);
}

ShardingState* ShardingState::get(OperationContext* operationContext) {
    return ShardingState::get(operationContext->getServiceContext());
}

bool ShardingState::enabled() const {
    return _getInitializationState() == InitializationState::kInitialized;
}

ConnectionString ShardingState::getConfigServer(OperationContext* txn) {
    invariant(enabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return grid.shardRegistry()->getConfigServerConnectionString();
}

string ShardingState::getShardName() {
    invariant(enabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _shardName;
}

void ShardingState::shutDown(OperationContext* txn) {
    bool mustEnterShutdownState = false;

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        while (_getInitializationState() == InitializationState::kInitializing) {
            _initializationFinishedCondition.wait(lk);
        }

        if (_getInitializationState() == InitializationState::kNew) {
            _setInitializationState_inlock(InitializationState::kInitializing);
            mustEnterShutdownState = true;
        }
    }

    // Initialization completion must be signalled outside of the mutex
    if (mustEnterShutdownState) {
        _signalInitializationComplete(
            Status(ErrorCodes::ShutdownInProgress,
                   "Sharding state unavailable because the system is shutting down"));
    }

    if (_getInitializationState() == InitializationState::kInitialized) {
        grid.getExecutorPool()->shutdownAndJoin();
        grid.catalogClient(txn)->shutDown(txn);
    }
}

Status ShardingState::updateConfigServerOpTimeFromMetadata(OperationContext* txn) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        // Nothing to do if we're a config server ourselves.
        return Status::OK();
    }

    boost::optional<repl::OpTime> opTime = rpc::ConfigServerMetadata::get(txn).getOpTime();
    if (opTime) {
        if (!AuthorizationSession::get(txn->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized to update config opTime");
        }

        grid.advanceConfigOpTime(*opTime);
    }

    return Status::OK();
}

void ShardingState::setShardName(const string& name) {
    const string clientAddr = cc().clientAddress(true);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_shardName.empty()) {
        // TODO SERVER-2299 remotely verify the name is sound w.r.t IPs
        _shardName = name;

        log() << "remote client " << clientAddr << " initialized this host as shard " << name;
        return;
    }

    if (_shardName != name) {
        const string message = str::stream()
            << "remote client " << clientAddr << " tried to initialize this host as shard " << name
            << ", but shard name was previously initialized as " << _shardName;

        warning() << message;
        uassertStatusOK({ErrorCodes::AlreadyInitialized, message});
    }
}

CollectionShardingState* ShardingState::getNS(const std::string& ns, OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    CollectionShardingStateMap::iterator it = _collections.find(ns);
    if (it == _collections.end()) {
        auto inserted =
            _collections.insert(make_pair(ns,
                                          stdx::make_unique<CollectionShardingState>(
                                              txn->getServiceContext(), NamespaceString(ns))));
        invariant(inserted.second);
        it = std::move(inserted.first);
    }

    return it->second.get();
}

void ShardingState::clearCollectionMetadata() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _collections.clear();
}

void ShardingState::resetMetadata(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    warning() << "resetting metadata for " << ns << ", this should only be used in testing";

    _collections.erase(ns);
}

void ShardingState::setGlobalInitMethodForTest(GlobalInitFunc func) {
    _globalInit = func;
}

void ShardingState::setScheduleCleanupFunctionForTest(RangeDeleterCleanupNotificationFunc fn) {
    _scheduleWorkFn = fn;
}

void ShardingState::scheduleCleanup(const NamespaceString& nss) {
    _scheduleWorkFn(nss);
}

Status ShardingState::onStaleShardVersion(OperationContext* txn,
                                          const NamespaceString& nss,
                                          const ChunkVersion& expectedVersion) {
    invariant(!txn->lockState()->isLocked());
    invariant(enabled());

    LOG(2) << "metadata refresh requested for " << nss.ns() << " at shard version "
           << expectedVersion;

    // Ensure any ongoing migrations have completed
    auto& oss = OperationShardingState::get(txn);
    oss.waitForMigrationCriticalSectionSignal(txn);

    ChunkVersion collectionShardVersion;

    // Fast path - check if the requested version is at a higher version than the current metadata
    // version or a different epoch before verifying against config server.
    ScopedCollectionMetadata currentMetadata;

    {
        AutoGetCollection autoColl(txn, nss, MODE_IS);

        currentMetadata = CollectionShardingState::get(txn, nss)->getMetadata();
        if (currentMetadata) {
            collectionShardVersion = currentMetadata->getShardVersion();
        }

        if (collectionShardVersion.epoch() == expectedVersion.epoch() &&
            collectionShardVersion >= expectedVersion) {
            // Don't need to remotely reload if we're in the same epoch and the requested version is
            // smaller than the one we know about. This means that the remote side is behind.
            return Status::OK();
        }
    }

    // At the first attempt try to use the currently loaded metadata and on subsequent attempts use
    // the complete metadata
    int numRefreshAttempts = 0;

    while (true) {
        numRefreshAttempts++;

        auto refreshStatusAndVersion =
            _refreshMetadata(txn, nss, (currentMetadata ? currentMetadata.getMetadata() : nullptr));
        if (refreshStatusAndVersion.isOK()) {
            LOG(1) << "Successfully refreshed metadata for " << nss.ns() << " to "
                   << refreshStatusAndVersion.getValue();
            return Status::OK();
        }

        if (refreshStatusAndVersion == ErrorCodes::RemoteChangeDetected &&
            numRefreshAttempts < kMaxNumMetadataRefreshAttempts) {
            currentMetadata = ScopedCollectionMetadata();

            log() << "Refresh failed and will be retried as full reload "
                  << refreshStatusAndVersion.getStatus();
            continue;
        }

        return refreshStatusAndVersion.getStatus();
    }


    MONGO_UNREACHABLE;
}

Status ShardingState::refreshMetadataNow(OperationContext* txn,
                                         const string& ns,
                                         ChunkVersion* latestShardVersion) {
    auto refreshLatestShardVersionStatus = _refreshMetadata(txn, NamespaceString(ns), nullptr);
    if (!refreshLatestShardVersionStatus.isOK()) {
        return refreshLatestShardVersionStatus.getStatus();
    }

    *latestShardVersion = refreshLatestShardVersionStatus.getValue();
    return Status::OK();
}

void ShardingState::initializeFromConfigConnString(OperationContext* txn, const string& configSvr) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_getInitializationState() == InitializationState::kNew) {
            uassert(18509,
                    "Unable to obtain host name during sharding initialization.",
                    !getHostName().empty());

            ConnectionString configSvrConnStr = uassertStatusOK(ConnectionString::parse(configSvr));

            _setInitializationState_inlock(InitializationState::kInitializing);

            stdx::thread thread([this, configSvrConnStr] { _initializeImpl(configSvrConnStr); });
            thread.detach();
        }
    }

    uassertStatusOK(_waitForInitialization(txn->getDeadline()));
    uassertStatusOK(reloadShardRegistryUntilSuccess(txn));
    uassertStatusOK(updateConfigServerOpTimeFromMetadata(txn));
}

// NOTE: This method can be called inside a database lock so it should never take any database
// locks, perform I/O, or any long running operations.
Status ShardingState::initializeFromShardIdentity(OperationContext* txn,
                                                  const ShardIdentityType& shardIdentity) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

    Status validationStatus = shardIdentity.validate();
    if (!validationStatus.isOK()) {
        return Status(
            validationStatus.code(),
            str::stream()
                << "Invalid shard identity document found when initializing sharding state: "
                << validationStatus.reason());
    }

    log() << "initializing sharding state with: " << shardIdentity;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // TODO: remove after v3.4.
    // This is for backwards compatibility with old style initialization through metadata
    // commands/setShardVersion, which can happen concurrently with an insert of a
    // shardIdentity document to admin.system.version.
    if (_getInitializationState() == InitializationState::kInitializing) {
        auto waitStatus = _waitForInitialization_inlock(Date_t::max(), lk);
        if (!waitStatus.isOK()) {
            return waitStatus;
        }
    }

    if (_getInitializationState() == InitializationState::kError) {
        return {ErrorCodes::ManualInterventionRequired,
                str::stream() << "Server's sharding metadata manager failed to initialize and will "
                                 "remain in this state until the instance is manually reset"
                              << causedBy(_initializationStatus)};
    }

    auto configSvrConnStr = shardIdentity.getConfigsvrConnString();

    // TODO: remove after v3.4.
    // This is for backwards compatibility with old style initialization through metadata
    // commands/setShardVersion, which sets the shardName and configsvrConnectionString.
    if (_getInitializationState() == InitializationState::kInitialized) {
        if (_shardName != shardIdentity.getShardName()) {
            return {ErrorCodes::InconsistentShardIdentity,
                    str::stream() << "shard name previously set as " << _shardName
                                  << " is different from stored: "
                                  << shardIdentity.getShardName()};
        }

        auto prevConfigsvrConnStr = grid.shardRegistry()->getConfigServerConnectionString();
        if (prevConfigsvrConnStr.type() != ConnectionString::SET) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "config server connection string was previously initialized as"
                                     " something that is not a replica set: "
                                  << prevConfigsvrConnStr.toString()};
        }

        if (prevConfigsvrConnStr.getSetName() != configSvrConnStr.getSetName()) {
            return {ErrorCodes::InconsistentShardIdentity,
                    str::stream() << "config server connection string previously set as "
                                  << prevConfigsvrConnStr.toString()
                                  << " is different from stored: "
                                  << configSvrConnStr.toString()};
        }

        // The clusterId will only be unset if sharding state was initialized via the sharding
        // metadata commands.
        if (!_clusterId.isSet()) {
            _clusterId = shardIdentity.getClusterId();
        } else if (_clusterId != shardIdentity.getClusterId()) {
            return {ErrorCodes::InconsistentShardIdentity,
                    str::stream() << "cluster id previously set as " << _clusterId
                                  << " is different from stored: "
                                  << shardIdentity.getClusterId()};
        }

        return Status::OK();
    }

    if (_getInitializationState() == InitializationState::kNew) {
        ShardedConnectionInfo::addHook();

        try {
            Status status = _globalInit(txn, configSvrConnStr, generateDistLockProcessId(txn));

            // TODO: remove after v3.4.
            // This is for backwards compatibility with old style initialization through metadata
            // commands/setShardVersion, which can happen concurrently with an insert of a
            // shardIdentity document to admin.system.version.
            if (status.isOK()) {
                _setInitializationState_inlock(InitializationState::kInitialized);
                ReplicaSetMonitor::setSynchronousConfigChangeHook(
                    &ConfigServer::replicaSetChangeShardRegistryUpdateHook);
                ReplicaSetMonitor::setAsynchronousConfigChangeHook(
                    &updateShardIdentityConfigStringCB);
            } else {
                _initializationStatus = status;
                _setInitializationState_inlock(InitializationState::kError);
            }

            _shardName = shardIdentity.getShardName();
            _clusterId = shardIdentity.getClusterId();

            _initializeRangeDeleterTaskExecutor();

            return status;
        } catch (const DBException& ex) {
            auto errorStatus = ex.toStatus();
            _setInitializationState_inlock(InitializationState::kError);
            _initializationStatus = errorStatus;
            return errorStatus;
        }
    }

    MONGO_UNREACHABLE;
}

void ShardingState::_initializeImpl(ConnectionString configSvr) {
    Client::initThread("ShardingState initialization");
    auto txn = cc().makeOperationContext();

    // Do this initialization outside of the lock, since we are already protected by having entered
    // the kInitializing state.
    ShardedConnectionInfo::addHook();

    try {
        Status status = _globalInit(txn.get(), configSvr, generateDistLockProcessId(txn.get()));

        if (status.isOK()) {
            ReplicaSetMonitor::setSynchronousConfigChangeHook(
                &ConfigServer::replicaSetChangeShardRegistryUpdateHook);
            ReplicaSetMonitor::setAsynchronousConfigChangeHook(&updateShardIdentityConfigStringCB);

            _initializeRangeDeleterTaskExecutor();
        }

        _signalInitializationComplete(status);

    } catch (const DBException& ex) {
        _signalInitializationComplete(ex.toStatus());
    }
}

Status ShardingState::_waitForInitialization(Date_t deadline) {
    if (enabled())
        return Status::OK();

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _waitForInitialization_inlock(deadline, lk);
}

Status ShardingState::_waitForInitialization_inlock(Date_t deadline,
                                                    stdx::unique_lock<stdx::mutex>& lk) {
    {
        while (_getInitializationState() == InitializationState::kInitializing ||
               _getInitializationState() == InitializationState::kNew) {
            if (deadline == Date_t::max()) {
                _initializationFinishedCondition.wait(lk);
            } else if (stdx::cv_status::timeout ==
                       _initializationFinishedCondition.wait_until(lk,
                                                                   deadline.toSystemTimePoint())) {
                return Status(ErrorCodes::ExceededTimeLimit,
                              "Initializing sharding state exceeded time limit");
            }
        }
    }

    auto initializationState = _getInitializationState();
    if (initializationState == InitializationState::kInitialized) {
        fassertStatusOK(34349, _initializationStatus);
        return Status::OK();
    }
    if (initializationState == InitializationState::kError) {
        return Status(ErrorCodes::ManualInterventionRequired,
                      str::stream()
                          << "Server's sharding metadata manager failed to initialize and will "
                             "remain in this state until the instance is manually reset"
                          << causedBy(_initializationStatus));
    }

    MONGO_UNREACHABLE;
}

ShardingState::InitializationState ShardingState::_getInitializationState() const {
    return static_cast<InitializationState>(_initializationState.load());
}

void ShardingState::_setInitializationState_inlock(InitializationState newState) {
    _initializationState.store(static_cast<uint32_t>(newState));
}

void ShardingState::_signalInitializationComplete(Status status) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    invariant(_getInitializationState() == InitializationState::kInitializing);

    if (!status.isOK()) {
        _initializationStatus = status;
        _setInitializationState_inlock(InitializationState::kError);
    } else {
        _initializationStatus = Status::OK();
        _setInitializationState_inlock(InitializationState::kInitialized);
    }

    _initializationFinishedCondition.notify_all();
}

Status ShardingState::initializeShardingAwarenessIfNeeded(OperationContext* txn) {
    // In sharded readOnly mode, we ignore the shardIdentity document on disk and instead *require*
    // a shardIdentity document to be passed through --overrideShardIdentity.
    if (storageGlobalParams.readOnly) {
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {ErrorCodes::InvalidOptions,
                        "If started with --shardsvr in queryableBackupMode, a shardIdentity "
                        "document must be provided through --overrideShardIdentity"};
            }
            auto swOverrideShardIdentity =
                ShardIdentityType::fromBSON(serverGlobalParams.overrideShardIdentity);
            if (!swOverrideShardIdentity.isOK()) {
                return swOverrideShardIdentity.getStatus();
            }
            auto status = initializeFromShardIdentity(txn, swOverrideShardIdentity.getValue());
            if (!status.isOK()) {
                return status;
            }
            return reloadShardRegistryUntilSuccess(txn);
        } else {
            // Error if --overrideShardIdentity is used but *not* started with --shardsvr.
            if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {
                    ErrorCodes::InvalidOptions,
                    str::stream()
                        << "Not started with --shardsvr, but a shardIdentity document was provided "
                           "through --overrideShardIdentity: "
                        << serverGlobalParams.overrideShardIdentity};
            }
            return Status::OK();
        }
    }
    // In sharded *non*-readOnly mode, error if --overrideShardIdentity is provided. Use the
    // shardIdentity document on disk if one exists, but it is okay if no shardIdentity document is
    // provided at all (sharding awareness will be initialized when a shardIdentity document is
    // inserted).
    else {
        if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "--overrideShardIdentity is only allowed in sharded "
                                 "queryableBackupMode. If not in queryableBackupMode, you can edit "
                                 "the shardIdentity document by starting the server *without* "
                                 "--shardsvr, manually updating the shardIdentity document in the "
                              << NamespaceString::kConfigCollectionNamespace.toString()
                              << " collection, and restarting the server with --shardsvr."};
        }

        // Load the shardIdentity document from disk.
        invariant(!txn->lockState()->isLocked());
        BSONObj shardIdentityBSON;
        try {
            AutoGetCollection autoColl(txn, NamespaceString::kConfigCollectionNamespace, MODE_IS);
            Helpers::findOne(txn,
                             autoColl.getCollection(),
                             BSON("_id" << ShardIdentityType::IdName),
                             shardIdentityBSON);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (shardIdentityBSON.isEmpty()) {
                warning() << "Started with --shardsvr, but no shardIdentity document was found on "
                             "disk in "
                          << NamespaceString::kConfigCollectionNamespace
                          << ". This most likely means this server has not yet been added to a "
                             "sharded cluster.";
                return Status::OK();
            }
            auto swShardIdentity = ShardIdentityType::fromBSON(shardIdentityBSON);
            if (!swShardIdentity.isOK()) {
                return swShardIdentity.getStatus();
            }
            auto status = initializeFromShardIdentity(txn, swShardIdentity.getValue());
            if (!status.isOK()) {
                return status;
            }
            return reloadShardRegistryUntilSuccess(txn);
        } else {
            // Warn if a shardIdentity document is found on disk but *not* started with --shardsvr.
            if (!shardIdentityBSON.isEmpty()) {
                warning() << "Not started with --shardsvr, but a shardIdentity document was found "
                             "on disk in "
                          << NamespaceString::kConfigCollectionNamespace << ": "
                          << shardIdentityBSON;
            }
            return Status::OK();
        }
    }
}

StatusWith<ChunkVersion> ShardingState::_refreshMetadata(
    OperationContext* txn, const NamespaceString& nss, const CollectionMetadata* metadataForDiff) {
    invariant(!txn->lockState()->isLocked());

    {
        Status status = _waitForInitialization(txn->getDeadline());
        if (!status.isOK())
            return status;
    }

    // We can't reload if a shard name has not yet been set
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_shardName.empty()) {
            string errMsg = str::stream() << "cannot refresh metadata for " << nss.ns()
                                          << " before shard name has been set";
            warning() << errMsg;
            return {ErrorCodes::NotYetInitialized, errMsg};
        }
    }

    // The _configServerTickets serializes this process such that only a small number of threads
    // can try to refresh at the same time
    _configServerTickets.waitForTicket();
    TicketHolderReleaser needTicketFrom(&_configServerTickets);

    LOG(1) << "Remotely refreshing metadata for " << nss.ns() << ", based on collection version "
           << (metadataForDiff ? metadataForDiff->getCollVersion().toString() : "(empty)");

    std::unique_ptr<CollectionMetadata> remoteMetadata(stdx::make_unique<CollectionMetadata>());

    {
        Timer refreshTimer;

        MetadataLoader mdLoader;
        Status status = mdLoader.makeCollectionMetadata(txn,
                                                        grid.catalogClient(txn),
                                                        nss.ns(),
                                                        getShardName(),
                                                        metadataForDiff,
                                                        remoteMetadata.get());

        if (status.code() == ErrorCodes::NamespaceNotFound) {
            remoteMetadata.reset();
        } else if (!status.isOK()) {
            warning() << "Could not remotely refresh metadata for " << nss.ns() << redact(status);

            return status;
        }
    }

    // Exclusive collection lock needed since we're now potentially changing the metadata, and
    // don't want reads/writes to be ongoing
    ScopedTransaction transaction(txn, MODE_IX);
    AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(txn, nss);

    // Resolve newer pending chunks with the remote metadata, finish construction
    css->refreshMetadata(txn, std::move(remoteMetadata));

    auto metadata = css->getMetadata();

    return (metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED());
}

StatusWith<ScopedRegisterMigration> ShardingState::registerMigration(const MoveChunkRequest& args) {
    if (_migrationDestManager.isActive()) {
        return {
            ErrorCodes::ConflictingOperationInProgress,
            str::stream()
                << "Unable start new migration because this shard is currently receiving a chunk"};
    }

    return _activeMigrationsRegistry.registerMigration(args);
}

boost::optional<NamespaceString> ShardingState::getActiveMigrationNss() {
    return _activeMigrationsRegistry.getActiveMigrationNss();
}

BSONObj ShardingState::getActiveMigrationStatusReport(OperationContext* txn) {
    return _activeMigrationsRegistry.getActiveMigrationStatusReport(txn);
}

void ShardingState::appendInfo(OperationContext* txn, BSONObjBuilder& builder) {
    const bool isEnabled = enabled();
    builder.appendBool("enabled", isEnabled);
    if (!isEnabled)
        return;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    builder.append("configServer",
                   grid.shardRegistry()->getConfigServerConnectionString().toString());
    builder.append("shardName", _shardName);
    builder.append("clusterId", _clusterId);

    BSONObjBuilder versionB(builder.subobjStart("versions"));
    for (CollectionShardingStateMap::const_iterator it = _collections.begin();
         it != _collections.end();
         ++it) {
        ScopedCollectionMetadata metadata = it->second->getMetadata();
        if (metadata) {
            versionB.appendTimestamp(it->first, metadata->getShardVersion().toLong());
        } else {
            versionB.appendTimestamp(it->first, ChunkVersion::UNSHARDED().toLong());
        }
    }

    versionB.done();
}

bool ShardingState::needCollectionMetadata(OperationContext* txn, const string& ns) {
    if (!enabled())
        return false;

    Client* client = txn->getClient();

    // Shard version information received from mongos may either by attached to the Client or
    // directly to the OperationContext.
    return ShardedConnectionInfo::get(client, false) ||
        OperationShardingState::get(txn).hasShardVersion();
}

Status ShardingState::updateShardIdentityConfigString(OperationContext* txn,
                                                      const std::string& newConnectionString) {
    BSONObj updateObj(ShardIdentityType::createConfigServerUpdateObject(newConnectionString));

    UpdateRequest updateReq(NamespaceString::kConfigCollectionNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdates(updateObj);
    UpdateLifecycleImpl updateLifecycle(NamespaceString::kConfigCollectionNamespace);
    updateReq.setLifecycle(&updateLifecycle);

    try {
        AutoGetOrCreateDb autoDb(txn, NamespaceString::kConfigCollectionNamespace.db(), MODE_X);

        auto result = update(txn, autoDb.getDb(), updateReq);
        if (result.numMatched == 0) {
            warning() << "failed to update config string of shard identity document because "
                      << "it does not exist. This shard could have been removed from the cluster";
        } else {
            LOG(2) << "Updated config server connection string in shardIdentity document to"
                   << newConnectionString;
        }
    } catch (const DBException& exception) {
        return exception.toStatus();
    }

    return Status::OK();
}

void ShardingState::_initializeRangeDeleterTaskExecutor() {
    invariant(!_rangeDeleterTaskExecutor);
    auto net =
        executor::makeNetworkInterface("NetworkInterfaceCollectionRangeDeleter-TaskExecutor");
    auto netPtr = net.get();
    _rangeDeleterTaskExecutor = stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        stdx::make_unique<executor::NetworkInterfaceThreadPool>(netPtr), std::move(net));
}

executor::ThreadPoolTaskExecutor* ShardingState::getRangeDeleterTaskExecutor() {
    return _rangeDeleterTaskExecutor.get();
}

/**
 * Global free function.
 */
bool isMongos() {
    return false;
}

}  // namespace mongo
