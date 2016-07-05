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
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
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

namespace {

const auto getShardingState = ServiceContext::declareDecoration<ShardingState>();

// Max number of concurrent config server refresh threads
const int kMaxConfigServerRefreshThreads = 3;

enum class VersionChoice { Local, Remote, Unknown };

/**
 * Compares a remotely-loaded version (remoteVersion) to the latest local version of a collection
 * (localVersion) and returns which one is the newest.
 *
 * Because it isn't clear during epoch changes which epoch is newer, the local version before the
 * reload occurred, 'prevLocalVersion', is used to determine whether the remote epoch is definitely
 * newer, or we're not sure.
 */
VersionChoice chooseNewestVersion(ChunkVersion prevLocalVersion,
                                  ChunkVersion localVersion,
                                  ChunkVersion remoteVersion) {
    OID prevEpoch = prevLocalVersion.epoch();
    OID localEpoch = localVersion.epoch();
    OID remoteEpoch = remoteVersion.epoch();

    // Everything changed in-flight, so we need to try again
    if (prevEpoch != localEpoch && localEpoch != remoteEpoch) {
        return VersionChoice::Unknown;
    }

    // We're in the same (zero) epoch as the latest metadata, nothing to do
    if (localEpoch == remoteEpoch && !remoteEpoch.isSet()) {
        return VersionChoice::Local;
    }

    // We're in the same (non-zero) epoch as the latest metadata, so increment the version
    if (localEpoch == remoteEpoch && remoteEpoch.isSet()) {
        // Use the newer version if possible
        if (localVersion < remoteVersion) {
            return VersionChoice::Remote;
        } else {
            return VersionChoice::Local;
        }
    }

    // We're now sure we're installing a new epoch and the epoch didn't change during reload
    dassert(prevEpoch == localEpoch && localEpoch != remoteEpoch);
    return VersionChoice::Remote;
}

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
                  << newConnectionString << causedBy(status);
    }
}

}  // namespace

//
// ShardingState
//

ShardingState::ShardingState()
    : _initializationState(static_cast<uint32_t>(InitializationState::kNew)),
      _initializationStatus(Status(ErrorCodes::InternalError, "Uninitialized value")),
      _configServerTickets(kMaxConfigServerRefreshThreads),
      _globalInit(&initializeGlobalShardingStateForMongod) {}

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

void ShardingState::updateConfigServerOpTimeFromMetadata(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        // Nothing to do if we're a config server ourselves.
        return;
    }

    boost::optional<repl::OpTime> opTime = rpc::ConfigServerMetadata::get(txn).getOpTime();
    if (opTime) {
        grid.advanceConfigOpTime(*opTime);
    }
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

CollectionShardingState* ShardingState::getNS(const std::string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    CollectionShardingStateMap::iterator it = _collections.find(ns);
    if (it == _collections.end()) {
        auto inserted = _collections.insert(make_pair(
            ns, stdx::make_unique<CollectionShardingState>(NamespaceString(ns), nullptr)));
        invariant(inserted.second);
        it = std::move(inserted.first);
    }

    return it->second.get();
}

void ShardingState::clearCollectionMetadata() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _collections.clear();
}

ChunkVersion ShardingState::getVersion(const string& ns) {
    ScopedCollectionMetadata p;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        CollectionShardingStateMap::const_iterator it = _collections.find(ns);
        if (it != _collections.end()) {
            p = it->second->getMetadata();
        }
    }

    if (p) {
        return p->getShardVersion();
    }

    return ChunkVersion::UNSHARDED();
}

void ShardingState::resetMetadata(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    warning() << "resetting metadata for " << ns << ", this should only be used in testing";

    _collections.erase(ns);
}

void ShardingState::setGlobalInitMethodForTest(GlobalInitFunc func) {
    _globalInit = func;
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
    {
        AutoGetCollection autoColl(txn, nss, MODE_IS);

        auto storedMetadata = CollectionShardingState::get(txn, nss)->getMetadata();
        if (storedMetadata) {
            collectionShardVersion = storedMetadata->getShardVersion();
        }

        if (collectionShardVersion >= expectedVersion &&
            collectionShardVersion.epoch() == expectedVersion.epoch()) {
            // Don't need to remotely reload if we're in the same epoch and the requested version is
            // smaller than the one we know about. This means that the remote side is behind.
            return Status::OK();
        }
    }

    // The _configServerTickets serializes this process such that only a small number of threads can
    // try to refresh at the same time
    _configServerTickets.waitForTicket();
    TicketHolderReleaser needTicketFrom(&_configServerTickets);

    //
    // Slow path - remotely reload
    //
    // Cases:
    // A) Initial config load and/or secondary take-over.
    // B) Migration TO this shard finished, notified by mongos.
    // C) Dropping a collection, notified (currently) by mongos.
    // D) Stale client wants to reload metadata with a different *epoch*, so we aren't sure.

    if (collectionShardVersion.epoch() != expectedVersion.epoch()) {
        // Need to remotely reload if our epochs aren't the same, to verify
        LOG(1) << "metadata change requested for " << nss.ns() << ", from shard version "
               << collectionShardVersion << " to " << expectedVersion
               << ", need to verify with config server";
    } else {
        // Need to remotely reload since our epochs aren't the same but our version is greater
        LOG(1) << "metadata version update requested for " << nss.ns() << ", from shard version "
               << collectionShardVersion << " to " << expectedVersion
               << ", need to verify with config server";
    }

    ChunkVersion unusedLatestShardVersion;
    return _refreshMetadata(txn, nss.ns(), expectedVersion, true, &unusedLatestShardVersion);
}

Status ShardingState::refreshMetadataNow(OperationContext* txn,
                                         const string& ns,
                                         ChunkVersion* latestShardVersion) {
    return _refreshMetadata(txn, ns, ChunkVersion(0, 0, OID()), false, latestShardVersion);
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
    updateConfigServerOpTimeFromMetadata(txn);
}

Status ShardingState::initializeFromShardIdentity(OperationContext* txn) {
    invariant(!txn->lockState()->isLocked());

    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return Status::OK();
    }

    BSONObj shardIdentityBSON;
    try {
        AutoGetCollection autoColl(txn, NamespaceString::kConfigCollectionNamespace, MODE_IS);
        if (!Helpers::findOne(txn,
                              autoColl.getCollection(),
                              BSON("_id"
                                   << "shardIdentity"),
                              shardIdentityBSON)) {
            return Status::OK();
        }
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    auto parseStatus = ShardIdentityType::fromBSON(shardIdentityBSON);
    if (!parseStatus.isOK()) {
        return parseStatus.getStatus();
    }

    auto status = initializeFromShardIdentity(txn, parseStatus.getValue(), txn->getDeadline());
    if (!status.isOK()) {
        return status;
    }

    return reloadShardRegistryUntilSuccess(txn);
}

// NOTE: This method can be called inside a database lock so it should never take any database
// locks, perform I/O, or any long running operations.
Status ShardingState::initializeFromShardIdentity(OperationContext* txn,
                                                  const ShardIdentityType& shardIdentity,
                                                  Date_t deadline) {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return Status::OK();
    }

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
    // commands/setShardVersion. As well as all assignments to _initializationStatus and
    // _setInitializationState_inlock in this method.
    if (_getInitializationState() == InitializationState::kInitializing) {
        auto waitStatus = _waitForInitialization_inlock(deadline, lk);
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

        // clusterId will only be unset if sharding state was initialized via the sharding
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

            // For backwards compatibility with old style inits from metadata commands.
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

Status ShardingState::_refreshMetadata(OperationContext* txn,
                                       const string& ns,
                                       const ChunkVersion& reqShardVersion,
                                       bool useRequestedVersion,
                                       ChunkVersion* latestShardVersion) {
    invariant(!txn->lockState()->isLocked());

    Status status = _waitForInitialization(txn->getDeadline());
    if (!status.isOK())
        return status;

    // We can't reload if a shard name has not yet been set
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_shardName.empty()) {
            string errMsg = str::stream() << "cannot refresh metadata for " << ns
                                          << " before shard name has been set";

            warning() << errMsg;
            return Status(ErrorCodes::NotYetInitialized, errMsg);
        }
    }

    const NamespaceString nss(ns);

    // The idea here is that we're going to reload the metadata from the config server, but we need
    // to do so outside any locks.  When we get our result back, if the current metadata has
    // changed, we may not be able to install the new metadata.
    ScopedCollectionMetadata beforeMetadata;
    {
        ScopedTransaction transaction(txn, MODE_IS);
        AutoGetCollection autoColl(txn, nss, MODE_IS);

        beforeMetadata = CollectionShardingState::get(txn, nss)->getMetadata();
    }

    ChunkVersion beforeShardVersion;
    ChunkVersion beforeCollVersion;

    if (beforeMetadata) {
        beforeShardVersion = beforeMetadata->getShardVersion();
        beforeCollVersion = beforeMetadata->getCollVersion();
    }

    *latestShardVersion = beforeShardVersion;

    //
    // Determine whether we need to diff or fully reload
    //

    bool fullReload = false;
    if (!beforeMetadata) {
        // We don't have any metadata to reload from
        fullReload = true;
    } else if (useRequestedVersion && reqShardVersion.epoch() != beforeShardVersion.epoch()) {
        // It's not useful to use the metadata as a base because we think the epoch will differ
        fullReload = true;
    }

    //
    // Load the metadata from the remote server, start construction
    //

    LOG(0) << "remotely refreshing metadata for " << ns
           << (useRequestedVersion
                   ? string(" with requested shard version ") + reqShardVersion.toString()
                   : "")
           << (fullReload ? ", current shard version is " : " based on current shard version ")
           << beforeShardVersion << ", current metadata version is " << beforeCollVersion;

    string errMsg;

    MetadataLoader mdLoader;
    std::unique_ptr<CollectionMetadata> remoteMetadata(stdx::make_unique<CollectionMetadata>());

    Timer refreshTimer;
    long long refreshMillis;

    {
        Status status =
            mdLoader.makeCollectionMetadata(txn,
                                            grid.catalogClient(txn),
                                            ns,
                                            getShardName(),
                                            fullReload ? nullptr : beforeMetadata.getMetadata(),
                                            remoteMetadata.get());
        refreshMillis = refreshTimer.millis();

        if (status.code() == ErrorCodes::NamespaceNotFound) {
            remoteMetadata.reset();
        } else if (!status.isOK()) {
            warning() << "could not remotely refresh metadata for " << ns
                      << causedBy(status.reason());

            return status;
        }
    }

    ChunkVersion remoteShardVersion;
    ChunkVersion remoteCollVersion;
    if (remoteMetadata) {
        remoteShardVersion = remoteMetadata->getShardVersion();
        remoteCollVersion = remoteMetadata->getCollVersion();
    }

    //
    // Get ready to install loaded metadata if needed
    //

    ScopedCollectionMetadata afterMetadata;
    ChunkVersion afterShardVersion;
    ChunkVersion afterCollVersion;
    VersionChoice choice;

    // If we choose to install the new metadata, this describes the kind of install
    enum InstallType {
        InstallType_New,
        InstallType_Update,
        InstallType_Replace,
        InstallType_Drop,
        InstallType_None
    } installType = InstallType_None;  // compiler complains otherwise

    {
        // Exclusive collection lock needed since we're now potentially changing the metadata,
        // and don't want reads/writes to be ongoing.
        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetCollection autoColl(txn, nss, MODE_IX, MODE_X);

        // Get the metadata now that the load has completed
        auto css = CollectionShardingState::get(txn, nss);
        afterMetadata = css->getMetadata();

        if (afterMetadata) {
            afterShardVersion = afterMetadata->getShardVersion();
            afterCollVersion = afterMetadata->getCollVersion();
        }

        *latestShardVersion = afterShardVersion;

        //
        // Resolve newer pending chunks with the remote metadata, finish construction
        //

        Status status =
            mdLoader.promotePendingChunks(afterMetadata.getMetadata(), remoteMetadata.get());
        if (!status.isOK()) {
            warning() << "remote metadata for " << ns
                      << " is inconsistent with current pending chunks"
                      << causedBy(status.reason());

            return status;
        }

        //
        // Compare the 'before', 'after', and 'remote' versions/epochs and choose newest
        // Zero-epochs (sentinel value for "dropped" collections), are tested by
        // !epoch.isSet().
        //

        choice = chooseNewestVersion(beforeCollVersion, afterCollVersion, remoteCollVersion);

        if (choice == VersionChoice::Remote) {
            dassert(
                !remoteCollVersion.epoch().isSet() || remoteShardVersion >= beforeShardVersion ||
                (remoteShardVersion.minorVersion() == 0 && remoteShardVersion.majorVersion() == 0));

            if (!afterCollVersion.epoch().isSet()) {
                // First metadata load
                installType = InstallType_New;
                css->setMetadata(std::move(remoteMetadata));
            } else if (remoteCollVersion.epoch().isSet() &&
                       remoteCollVersion.epoch() == afterCollVersion.epoch()) {
                // Update to existing metadata
                installType = InstallType_Update;
                css->setMetadata(std::move(remoteMetadata));
            } else if (remoteCollVersion.epoch().isSet()) {
                // New epoch detected, replacing metadata
                installType = InstallType_Replace;
                css->setMetadata(std::move(remoteMetadata));
            } else {
                dassert(!remoteCollVersion.epoch().isSet());

                // Drop detected
                installType = InstallType_Drop;
                css->setMetadata(nullptr);
            }

            *latestShardVersion = remoteShardVersion;
        }
    }
    // End _mutex
    // End DBWrite

    //
    // Do messaging based on what happened above
    //
    string localShardVersionMsg = beforeShardVersion.epoch() == afterShardVersion.epoch()
        ? afterShardVersion.toString()
        : beforeShardVersion.toString() + " / " + afterShardVersion.toString();

    if (choice == VersionChoice::Unknown) {
        string errMsg = str::stream()
            << "need to retry loading metadata for " << ns
            << ", collection may have been dropped or recreated during load"
            << " (loaded shard version : " << remoteShardVersion.toString()
            << ", stored shard versions : " << localShardVersionMsg << ", took " << refreshMillis
            << "ms)";

        warning() << errMsg;
        return Status(ErrorCodes::RemoteChangeDetected, errMsg);
    }

    if (choice == VersionChoice::Local) {
        LOG(0) << "metadata of collection " << ns
               << " already up to date (shard version : " << afterShardVersion.toString()
               << ", took " << refreshMillis << "ms)";
        return Status::OK();
    }

    dassert(choice == VersionChoice::Remote);

    switch (installType) {
        case InstallType_New:
            LOG(0) << "collection " << ns << " was previously unsharded"
                   << ", new metadata loaded with shard version " << remoteShardVersion;
            break;
        case InstallType_Update:
            LOG(0) << "updating metadata for " << ns << " from shard version "
                   << localShardVersionMsg << " to shard version " << remoteShardVersion;
            break;
        case InstallType_Replace:
            LOG(0) << "replacing metadata for " << ns << " at shard version "
                   << localShardVersionMsg << " with a new epoch (shard version "
                   << remoteShardVersion << ")";
            break;
        case InstallType_Drop:
            LOG(0) << "dropping metadata for " << ns << " at shard version " << localShardVersionMsg
                   << ", took " << refreshMillis << "ms";
            break;
        default:
            verify(false);
            break;
    }

    if (installType != InstallType_Drop) {
        LOG(0) << "collection version was loaded at version " << remoteCollVersion << ", took "
               << refreshMillis << "ms";
    }

    return Status::OK();
}

StatusWith<ScopedRegisterMigration> ShardingState::registerMigration(const MoveChunkRequest& args) {
    return _activeMigrationsRegistry.registerMigration(args);
}

boost::optional<NamespaceString> ShardingState::getActiveMigrationNss() {
    return _activeMigrationsRegistry.getActiveMigrationNss();
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

ScopedCollectionMetadata ShardingState::getCollectionMetadata(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CollectionShardingStateMap::const_iterator it = _collections.find(ns);
    invariant(it != _collections.end());
    return it->second->getMetadata();
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

/**
 * Global free function.
 */
bool isMongos() {
    return false;
}

}  // namespace mongo
