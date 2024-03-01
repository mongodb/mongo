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


#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>
#include <mutex>
#include <tuple>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replica_set_endpoint_sharding_state.h"
#include "mongo/db/replica_set_endpoint_util.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/read_only_catalog_cache_loader.h"
#include "mongo/db/s/shard_local.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_ready.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/s/sharding_state.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

// Failpoint for disabling updateShardIdentityConfigString calls on signaled RS nodes.
MONGO_FAIL_POINT_DEFINE(failUpdateShardIdentityConfigString);
MONGO_FAIL_POINT_DEFINE(hangDuringShardingInitialization);

namespace {

const auto getInstance = ServiceContext::declareDecoration<ShardingInitializationMongoD>();

const ReplicaSetAwareServiceRegistry::Registerer<ShardingInitializationMongoD> _registryRegisterer(
    "ShardingInitializationMongoDRegistry");

/**
 * Updates the config server field of the shardIdentity document with the given connection string if
 * setName is equal to the config server replica set name.
 */
class ShardingReplicaSetChangeListener final
    : public ReplicaSetChangeNotifier::Listener,
      public std::enable_shared_from_this<ShardingReplicaSetChangeListener> {
public:
    ShardingReplicaSetChangeListener(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}
    ~ShardingReplicaSetChangeListener() final = default;

    void onFoundSet(const Key&) noexcept final {}

    // Update the shard identy config string
    void onConfirmedSet(const State& state) noexcept final {
        const auto& connStr = state.connStr;
        try {
            LOGV2(471691,
                  "Updating the shard registry with confirmed replica set",
                  "connectionString"_attr = connStr);
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kConfirmed);
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
            LOGV2(471692, "Unable to update the shard registry", "error"_attr = e);
        }

        auto const shardingState = ShardingState::get(_serviceContext);
        if (!shardingState->enabled()) {
            // If our sharding state isn't enabled, we don't have a shard identity document, so
            // there's nothing to update. Note technically this may race with the config server
            // being added as a shard, but that shouldn't be a problem since addShard will use a
            // valid connection string and should serialize with a replica set reconfig.
            return;
        }

        const auto& setName = connStr.getSetName();
        bool updateInProgress = false;
        {
            stdx::lock_guard lock(_mutex);
            if (!_hasUpdateState(lock, setName)) {
                _updateStates.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(setName),
                                      std::forward_as_tuple());
            }

            auto& updateState = _updateStates.at(setName);
            updateState.nextUpdateToSend = connStr;
            updateInProgress = updateState.updateInProgress;
        }

        if (!updateInProgress) {
            _scheduleUpdateShardIdentityConfigString(setName);
        }
    }

    void onPossibleSet(const State& state) noexcept final {
        try {
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(state.connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kPossible);
        } catch (const DBException& ex) {
            LOGV2_DEBUG(22070,
                        2,
                        "Unable to update config server with possible replica set",
                        "error"_attr = ex);
        }
    }

    void onDroppedSet(const Key&) noexcept final {}

private:
    // Schedules updates to the shard identity config string while preserving order.
    void _scheduleUpdateShardIdentityConfigString(const std::string& setName) {
        ConnectionString updatedConnectionString;
        {
            stdx::lock_guard lock(_mutex);
            if (!_hasUpdateState(lock, setName)) {
                return;
            }
            auto& updateState = _updateStates.at(setName);
            if (updateState.updateInProgress) {
                return;
            }
            updateState.updateInProgress = true;
            updatedConnectionString = updateState.nextUpdateToSend.value();
            updateState.nextUpdateToSend = boost::none;
        }

        auto executor = Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();
        executor->schedule([self = shared_from_this(),
                            setName,
                            update = std::move(updatedConnectionString)](const Status& status) {
            self->_updateShardIdentityConfigString(status, setName, update);
        });
    }

    void _updateShardIdentityConfigString(const Status& status,
                                          const std::string& setName,
                                          const ConnectionString& update) {
        if (ErrorCodes::isCancellationError(status.code())) {
            LOGV2_DEBUG(
                22067, 2, "Unable to schedule confirmed replica set update", "error"_attr = status);
            stdx::lock_guard lk(_mutex);
            _updateStates.erase(setName);
            return;
        }
        invariant(status);

        if (MONGO_unlikely(failUpdateShardIdentityConfigString.shouldFail())) {
            _endUpdateShardIdentityConfigString(setName, update);
            return;
        }

        auto configsvrConnStr =
            Grid::get(_serviceContext)->shardRegistry()->getConfigServerConnectionString();

        // Only proceed if the notification is for the configsvr.
        if (configsvrConnStr.getSetName() != update.getSetName()) {
            _endUpdateShardIdentityConfigString(setName, update);
            return;
        }

        try {
            LOGV2(22068,
                  "Updating shard identity config string with confirmed replica set",
                  "connectionString"_attr = update);


            ThreadClient tc("updateShardIdentityConfigString",
                            _serviceContext->getService(ClusterRole::ShardServer));

            // TODO(SERVER-74658): Please revisit if this thread could be made killable.
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc.get()->setSystemOperationUnkillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();
            ShardingInitializationMongoD::updateShardIdentityConfigString(opCtx.get(), update);
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
            LOGV2(22069, "Unable to update shard identity config string", "error"_attr = e);
        } catch (...) {
            _endUpdateShardIdentityConfigString(setName, update);
            throw;
        }
        _endUpdateShardIdentityConfigString(setName, update);
    }

    void _endUpdateShardIdentityConfigString(const std::string& setName,
                                             const ConnectionString& update) {
        bool moreUpdates = false;
        {
            stdx::lock_guard lock(_mutex);
            invariant(_hasUpdateState(lock, setName));
            auto& updateState = _updateStates.at(setName);
            updateState.updateInProgress = false;
            moreUpdates = (updateState.nextUpdateToSend != boost::none);
            if (!moreUpdates) {
                _updateStates.erase(setName);
            }
        }
        if (moreUpdates) {
            auto executor = Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();
            executor->schedule([self = shared_from_this(), setName](const auto& _) {
                self->_scheduleUpdateShardIdentityConfigString(setName);
            });
        }
    }

    // Returns true if a ReplSetConfigUpdateState exists for replica set setName.
    bool _hasUpdateState(WithLock, const std::string& setName) {
        return (_updateStates.find(setName) != _updateStates.end());
    }

    ServiceContext* _serviceContext;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ShardingReplicaSetChangeListenerMongod::mutex");

    struct ReplSetConfigUpdateState {
        ReplSetConfigUpdateState() = default;
        ReplSetConfigUpdateState(const ReplSetConfigUpdateState&) = delete;
        ReplSetConfigUpdateState& operator=(const ReplSetConfigUpdateState&) = delete;

        bool updateInProgress = false;
        boost::optional<ConnectionString> nextUpdateToSend;
    };

    stdx::unordered_map<std::string, ReplSetConfigUpdateState> _updateStates;
};

/**
 * Internal function to initialize the global objects that define the sharding state given an
 * operation context and a config server connection string.
 */
void _initializeGlobalShardingState(OperationContext* opCtx,
                                    const boost::optional<ConnectionString>& configCS) {
    if (configCS) {
        uassert(ErrorCodes::BadValue, "Unrecognized connection string.", *configCS);
    }

    auto targeterFactory = std::make_unique<RemoteCommandTargeterFactoryImpl>();
    auto targeterFactoryPtr = targeterFactory.get();

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::ConnectionType::kReplicaSet,
         [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
             return std::make_unique<ShardRemote>(
                 shardId, connStr, targeterFactoryPtr->create(connStr));
         }},
        {ConnectionString::ConnectionType::kLocal,
         [](const ShardId& shardId, const ConnectionString& connStr) {
             return std::make_unique<ShardLocal>(shardId);
         }},
        {ConnectionString::ConnectionType::kStandalone,
         [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
             return std::make_unique<ShardRemote>(
                 shardId, connStr, targeterFactoryPtr->create(connStr));
         }},
    };

    auto shardFactory =
        std::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto const service = opCtx->getServiceContext();

    auto validator = LogicalTimeValidator::get(service);
    if (validator) {  // The keyManager may be existing if the node was a part of a standalone RS.
        validator->stopKeyManager();
    }

    globalConnPool.addHook(new ShardingConnectionHook(makeShardingEgressHooksList(service)));

    auto catalogCache = std::make_unique<CatalogCache>(service, CatalogCacheLoader::get(opCtx));

    // List of hooks which will be called by the ShardRegistry when it discovers a shard has been
    // removed.
    std::vector<ShardRegistry::ShardRemovalHook> shardRemovalHooks = {
        // Invalidate appropriate entries in the CatalogCache when a shard is removed. It's safe to
        // capture the CatalogCache pointer since the Grid (and therefore CatalogCache and
        // ShardRegistry) are never destroyed.
        [catCache = catalogCache.get()](const ShardId& removedShard) {
            catCache->invalidateEntriesThatReferenceShard(removedShard);
        }};

    auto shardRegistry = std::make_unique<ShardRegistry>(
        service, std::move(shardFactory), configCS, std::move(shardRemovalHooks));

    auto initKeysClient =
        [service](ShardingCatalogClient* catalogClient) -> std::unique_ptr<KeysCollectionClient> {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // The direct keys client must use local read concern if the storage engine can't
            // support majority read concern.
            bool keysClientMustUseLocalReads =
                !service->getStorageEngine()->supportsReadConcernMajority();
            return std::make_unique<KeysCollectionClientDirect>(keysClientMustUseLocalReads);
        }
        return std::make_unique<KeysCollectionClientSharded>(catalogClient);
    };

    uassertStatusOK(initializeGlobalShardingState(
        opCtx,
        std::move(catalogCache),
        std::move(shardRegistry),
        [service] { return makeShardingEgressHooksList(service); },
        // We only need one task executor here because sharding task
        // executors aren't used for user queries in mongod.
        1,
        initKeysClient));

    if (replica_set_endpoint::isFeatureFlagEnabledIgnoreFCV() &&
        serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // The feature flag check here needs to ignore the FCV since the
        // ReplicaSetEndpointShardingState needs to be maintained even before the FCV is fully
        // upgraded.
        DBDirectClient client(opCtx);
        FindCommandRequest request(NamespaceString::kConfigsvrShardsNamespace);
        request.setFilter(BSON("_id" << ShardId::kConfigServerId));
        auto cursor = client.find(request);
        if (cursor->more()) {
            replica_set_endpoint::ReplicaSetEndpointShardingState::get(opCtx)->setIsConfigShard(
                true);
        }
    }

    auto const replCoord = repl::ReplicationCoordinator::get(service);
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
        replCoord->getMemberState().primary()) {
        LogicalTimeValidator::get(opCtx)->enableKeyGenerator(opCtx, true);
    }
}

}  // namespace

ShardingInitializationMongoD::ShardingInitializationMongoD()
    : _initFunc([this](auto... args) {
          _initializeShardingEnvironmentOnShardServer(std::forward<decltype(args)>(args)...);
      }) {}

ShardingInitializationMongoD::~ShardingInitializationMongoD() = default;

ShardingInitializationMongoD* ShardingInitializationMongoD::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

ShardingInitializationMongoD* ShardingInitializationMongoD::get(ServiceContext* service) {
    return &getInstance(service);
}

void ShardingInitializationMongoD::shutDown(OperationContext* opCtx) {
    auto const shardingState = ShardingState::get(opCtx);
    if (!shardingState->enabled())
        return;

    _replicaSetChangeListener.reset();
}

void ShardingInitializationMongoD::initializeFromShardIdentity(
    OperationContext* opCtx, const ShardIdentityType& shardIdentity) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
    invariant(shard_role_details::getLocker(opCtx)->isLocked());

    uassertStatusOKWithContext(
        shardIdentity.validate(),
        "Invalid shard identity document found when initializing sharding state");

    LOGV2(22072, "Initializing sharding state", "initialShardIdentity"_attr = shardIdentity);

    const auto& configSvrConnStr = shardIdentity.getConfigsvrConnectionString();

    auto const shardingState = ShardingState::get(opCtx);

    hangDuringShardingInitialization.pauseWhileSet();

    stdx::unique_lock<Latch> ul(_initSynchronizationMutex);

    if (shardingState->enabled()) {
        uassert(40371,
                "Existing shard id does not match shard identity shard id",
                shardingState->shardId() == shardIdentity.getShardName());
        uassert(40372,
                "Existing cluster id does not match shard identity cluster id",
                shardingState->clusterId() == shardIdentity.getClusterId());

        // If run on a config server, we may not know our connection string yet.
        if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
            auto prevConfigsvrConnStr = shardRegistry->getConfigServerConnectionString();
            uassert(
                40373,
                "Existing config server connection string is unexpectedly not for a replica set",
                prevConfigsvrConnStr.type() == ConnectionString::ConnectionType::kReplicaSet);
            uassert(40374,
                    "Existing config server connection string set name does not match shard "
                    "identity config server connection string set name",
                    prevConfigsvrConnStr.getSetName() == configSvrConnStr.getSetName());
        }

        return;
    }

    try {
        _initFunc(opCtx, shardIdentity);
        shardingState->setRecoveryCompleted({shardIdentity.getClusterId(),
                                             serverGlobalParams.clusterRole,
                                             shardIdentity.getConfigsvrConnectionString(),
                                             ShardId(shardIdentity.getShardName().toString())});
    } catch (const DBException& ex) {
        shardingState->setRecoveryFailed(ex.toStatus());
    }

    if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        Grid::get(opCtx)->setShardingInitialized();
    } else {
        // A config server always initializes sharding at startup.
        invariant(Grid::get(opCtx)->isShardingInitialized());
    }

    if (audit::initializeSynchronizeJob) {
        audit::initializeSynchronizeJob(opCtx->getServiceContext());
    }
}

void ShardingInitializationMongoD::updateShardIdentityConfigString(
    OperationContext* opCtx, const ConnectionString& newConnectionString) {
    BSONObj updateObj(
        ShardIdentityType::createConfigServerUpdateObject(newConnectionString.toString()));

    auto updateReq = UpdateRequest();
    updateReq.setNamespaceString(NamespaceString::kServerConfigurationNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(updateObj));

    try {
        auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(NamespaceString::kServerConfigurationNamespace,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
        auto result = update(opCtx, collection, updateReq);
        if (result.numMatched == 0) {
            LOGV2_WARNING(22076,
                          "Failed to update config server connection string of shard identity "
                          "document because it does not exist. This shard could have been removed "
                          "from the cluster");
        } else {
            LOGV2_DEBUG(22073,
                        2,
                        "Updated config server connection string in shardIdentity document",
                        "newConnectionString"_attr = newConnectionString);
        }
    } catch (const DBException& exception) {
        auto status = exception.toStatus();
        if (!ErrorCodes::isNotPrimaryError(status.code())) {
            LOGV2_WARNING(22077,
                          "Error encountered while trying to update config connection string",
                          "newConnectionString"_attr = newConnectionString.toString(),
                          "error"_attr = redact(status));
        }
    }
}

void ShardingInitializationMongoD::onStepUpBegin(OperationContext* opCtx, long long term) {
    _isPrimary.store(true);
    if (Grid::get(opCtx)->isInitialized()) {
        auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
        // Update the replica set connection string in the config.shards document for this shard.
        // Wait for the cluster to be fully initialized to avoid a race where an auto-bootstrapped
        // config shard finishes initializing after the update finishes (resulting in no updates).
        (void)ShardingReady::get(opCtx)->isReadyFuture().thenRunOn(executor).then([&]() {
            ShardRegistry::scheduleReplicaSetUpdateOnConfigServerIfNeeded(
                [&]() -> bool { return _isPrimary.load(); });
        });
    }
}

void ShardingInitializationMongoD::onStepDown() {
    _isPrimary.store(false);
}

void ShardingInitializationMongoD::onSetCurrentConfig(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // TODO: SERVER-82965 Remove if condition, shard registry should always exists for config
        // servers.
        if (auto shardRegistry = Grid::get(opCtx)->shardRegistry()) {
            auto myConnectionString =
                repl::ReplicationCoordinator::get(opCtx)->getConfigConnectionString();
            shardRegistry->initConfigShardIfNecessary(myConnectionString);
        }
    }
    if (Grid::get(opCtx)->isInitialized()) {
        ShardRegistry::scheduleReplicaSetUpdateOnConfigServerIfNeeded(
            [&]() -> bool { return _isPrimary.load(); });
    }
}

void ShardingInitializationMongoD::onInitialDataAvailable(OperationContext* opCtx,
                                                          bool isMajorityDataAvailable) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        initializeGlobalShardingStateForMongoD(opCtx);
    }

    if (auto shardIdentityDoc = getShardIdentityDoc(opCtx)) {
        // This function will take the global lock.
        initializeShardingAwarenessAndLoadGlobalSettings(opCtx, *shardIdentityDoc);
    }
}

void initializeGlobalShardingStateForMongoD(OperationContext* opCtx) {
    if (Grid::get(opCtx)->isShardingInitialized()) {
        return;
    }

    const auto service = opCtx->getServiceContext();

    auto configCS = []() -> boost::optional<ConnectionString> {
        // When the config server can operate as a shard, it sets up a ShardRemote for the
        // config shard, which is created later after loading the local replica set config.
        return boost::none;
    }();

    // (Ignore FCV check): TODO(SERVER-75389): add why FCV is ignored here.
    if (gFeatureFlagTransitionToCatalogShard.isEnabledAndIgnoreFCVUnsafe()) {
        CatalogCacheLoader::set(service,
                                std::make_unique<ShardServerCatalogCacheLoader>(
                                    std::make_unique<ConfigServerCatalogCacheLoader>()));
        {
            // This is only called in startup when there shouldn't be replication state changes, but
            // to be safe we take the RSTL anyway.
            repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            bool isReplSet = replCoord->getSettings().isReplSet();
            bool isStandaloneOrPrimary =
                !isReplSet || (replCoord->getMemberState() == repl::MemberState::RS_PRIMARY);
            CatalogCacheLoader::get(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);
        }
    } else {
        CatalogCacheLoader::set(service, std::make_unique<ConfigServerCatalogCacheLoader>());
    }

    _initializeGlobalShardingState(opCtx, configCS);

    ShardingInitializationMongoD::get(opCtx)->installReplicaSetChangeListener(service);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // ShardLocal to use for explicitly local commands on the config server.
        auto localConfigShard = Grid::get(opCtx)->shardRegistry()->createLocalConfigShard();
        auto localCatalogClient = std::make_unique<ShardingCatalogClientImpl>(localConfigShard);

        ShardingCatalogManager::create(
            service,
            makeShardingTaskExecutor(executor::makeNetworkInterface("AddShard-TaskExecutor")),
            std::move(localConfigShard),
            std::move(localCatalogClient));
    }

    Grid::get(opCtx)->setShardingInitialized();
}

void ShardingInitializationMongoD::installReplicaSetChangeListener(ServiceContext* service) {
    _replicaSetChangeListener =
        ReplicaSetMonitor::getNotifier().makeListener<ShardingReplicaSetChangeListener>(service);
}

void ShardingInitializationMongoD::_initializeShardingEnvironmentOnShardServer(
    OperationContext* opCtx, const ShardIdentity& shardIdentity) {
    auto const service = opCtx->getServiceContext();

    // Determine primary/secondary/standalone state in order to properly initialize sharding
    // components.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getSettings().isReplSet();
    bool isStandaloneOrPrimary =
        !isReplSet || (replCoord->getMemberState() == repl::MemberState::RS_PRIMARY);

    if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // A config server added as a shard would have already set this up at startup.
        if (storageGlobalParams.queryableBackupMode) {
            CatalogCacheLoader::set(service, std::make_unique<ReadOnlyCatalogCacheLoader>());
        } else {
            CatalogCacheLoader::set(service,
                                    std::make_unique<ShardServerCatalogCacheLoader>(
                                        std::make_unique<ConfigServerCatalogCacheLoader>()));
        }

        CatalogCacheLoader::get(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);
        _initializeGlobalShardingState(opCtx, {shardIdentity.getConfigsvrConnectionString()});

        installReplicaSetChangeListener(service);

        // Reset the shard register config connection string in case it missed the replica set
        // monitor notification. Config server does not need to do this since it gets the connection
        // string directly from the replication coordinator.
        auto configShardConnStr = Grid::get(opCtx->getServiceContext())
                                      ->shardRegistry()
                                      ->getConfigServerConnectionString();
        if (configShardConnStr.type() == ConnectionString::ConnectionType::kReplicaSet) {
            ConnectionString rsMonitorConfigConnStr(
                ReplicaSetMonitor::get(configShardConnStr.getSetName())->getServerAddress(),
                ConnectionString::ConnectionType::kReplicaSet);
            Grid::get(opCtx->getServiceContext())
                ->shardRegistry()
                ->updateReplSetHosts(rsMonitorConfigConnStr,
                                     ShardRegistry::ConnectionStringUpdateType::kConfirmed);
        }

        if (auto routerService = service->getService(ClusterRole::RouterServer); routerService) {
            uassertStatusOK(AuthorizationManager::get(routerService)->initialize(opCtx));
            UserCacheInvalidator::start(service, opCtx);
        }
    }

    // Start transaction coordinator service only if the node is the primary of a replica set.
    TransactionCoordinatorService::get(opCtx)->onShardingInitialization(
        opCtx, isReplSet && isStandaloneOrPrimary);

    LOGV2(22071,
          "Finished initializing sharding components",
          "memberState"_attr = (isStandaloneOrPrimary ? "primary" : "secondary"));
}

void initializeShardingAwarenessAndLoadGlobalSettings(OperationContext* opCtx,
                                                      const ShardIdentity& shardIdentity,
                                                      BSONObjBuilder* startupTimeElapsedBuilder) {
    {
        auto scopedTimer = createTimeElapsedBuilderScopedTimer(
            opCtx->getServiceContext()->getFastClockSource(),
            "Initialize information needed to make a mongod instance shard aware",
            startupTimeElapsedBuilder);
        Lock::GlobalWrite lk(opCtx);
        ShardingInitializationMongoD::get(opCtx)->initializeFromShardIdentity(opCtx, shardIdentity);
    }

    {
        // Config servers can't always perform remote reads here, so they use a local client.
        auto scopedTimer =
            createTimeElapsedBuilderScopedTimer(opCtx->getServiceContext()->getFastClockSource(),
                                                "Load global settings from config server",
                                                startupTimeElapsedBuilder);
        auto catalogClient = serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)
            ? ShardingCatalogManager::get(opCtx)->localCatalogClient()
            : Grid::get(opCtx)->catalogClient();
        auto status = loadGlobalSettingsFromConfigServer(opCtx, catalogClient);
        if (!status.isOK()) {
            LOGV2_ERROR(20545,
                        "Error loading global settings from config server at startup",
                        "error"_attr = redact(status));
        }
    }
}

boost::optional<ShardIdentity> ShardingInitializationMongoD::getShardIdentityDoc(
    OperationContext* opCtx) {
    // In sharded queryableBackupMode mode, we ignore the shardIdentity document on disk and instead
    // *require* a shardIdentity document to be passed through --overrideShardIdentity
    if (storageGlobalParams.queryableBackupMode) {
        if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::ShardServer)) {
            uassert(ErrorCodes::InvalidOptions,
                    "If started with --shardsvr in queryableBackupMode, a shardIdentity document "
                    "must be provided through --overrideShardIdentity",
                    !serverGlobalParams.overrideShardIdentity.isEmpty());

            return uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(
                serverGlobalParams.overrideShardIdentity));
        } else {
            // Error if --overrideShardIdentity is used but *not* started with --shardsvr
            uassert(ErrorCodes::InvalidOptions,
                    str::stream()
                        << "Not started with --shardsvr, but a shardIdentity document was provided "
                           "through --overrideShardIdentity: "
                        << serverGlobalParams.overrideShardIdentity,
                    serverGlobalParams.overrideShardIdentity.isEmpty());
            return boost::none;
        }

        MONGO_UNREACHABLE;
    }

    // In sharded *non*-readOnly mode, error if --overrideShardIdentity is provided
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "--overrideShardIdentity is only allowed in sharded "
                             "queryableBackupMode. If not in queryableBackupMode, you can edit "
                             "the shardIdentity document by starting the server *without* "
                             "--shardsvr, manually updating the shardIdentity document in the "
                          << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg()
                          << " collection, and restarting the server with --shardsvr.",
            serverGlobalParams.overrideShardIdentity.isEmpty());

    // Use the shardIdentity document on disk if one exists, but it is okay if no shardIdentity
    // document is available at all (sharding awareness will be initialized when a shardIdentity
    // document is inserted)
    BSONObj shardIdentityBSON;
    const bool foundShardIdentity = [&] {
        AutoGetCollection autoColl(opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IS);
        return Helpers::findOne(opCtx,
                                autoColl.getCollection(),
                                BSON("_id" << ShardIdentityType::IdName),
                                shardIdentityBSON);
    }();

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        if (!foundShardIdentity) {
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                LOGV2_WARNING(7445900,
                              "Started with ConfigServer role, but no shardIdentity document was "
                              "found on disk.",
                              logAttrs(NamespaceString::kServerConfigurationNamespace));
            } else {
                LOGV2_WARNING(22074,
                              "Started with ShardServer role, but no shardIdentity document was "
                              "found on disk. This most likely means this server has not yet been "
                              "added to a sharded cluster.",
                              logAttrs(NamespaceString::kServerConfigurationNamespace));
            }
            return boost::none;
        }

        invariant(!shardIdentityBSON.isEmpty());

        auto shardIdentity =
            uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(shardIdentityBSON));
        uassertStatusOK(shardIdentity.validate(
            true /* fassert cluster role matches shard identity document */));

        return shardIdentity;
    } else {
        // Warn if a shardIdentity document is found on disk but *not* started with --shardsvr.
        if (!shardIdentityBSON.isEmpty()) {
            LOGV2_WARNING(
                22075,
                "Not started with --shardsvr, but a shardIdentity document was found on disk",
                logAttrs(NamespaceString::kServerConfigurationNamespace),
                "shardIdentityDocument"_attr = shardIdentityBSON);
        }

        return boost::none;
    }
}

}  // namespace mongo
