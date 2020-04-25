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

#include "mongo/db/s/sharding_initialization_mongod.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client_metadata_propagation_egress_hook.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/periodic_balancer_config_refresher.h"
#include "mongo/db/s/read_only_catalog_cache_loader.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/sharding_config_optime_gossip.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_local.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/exit.h"

namespace mongo {

// Failpoint for disabling updateShardIdentityConfigString calls on signaled RS nodes.
MONGO_FAIL_POINT_DEFINE(failUpdateShardIdentityConfigString);

namespace {

const auto getInstance = ServiceContext::declareDecoration<ShardingInitializationMongoD>();

auto makeEgressHooksList(ServiceContext* service) {
    auto unshardedHookList = std::make_unique<rpc::EgressMetadataHookList>();
    unshardedHookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(service));
    unshardedHookList->addHook(std::make_unique<rpc::ClientMetadataPropagationEgressHook>());
    unshardedHookList->addHook(std::make_unique<rpc::ShardingEgressMetadataHookForMongod>(service));

    return unshardedHookList;
}

/**
 * Updates the config server field of the shardIdentity document with the given connection string if
 * setName is equal to the config server replica set name.
 */
class ShardingReplicaSetChangeListener final : public ReplicaSetChangeNotifier::Listener {
public:
    ShardingReplicaSetChangeListener(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}
    ~ShardingReplicaSetChangeListener() final = default;

    void onFoundSet(const Key&) noexcept final {}

    // Update the shard identy config string
    void onConfirmedSet(const State& state) noexcept final {
        auto connStr = state.connStr;
        try {
            LOGV2(471691,
                  "Updating the shard registry with confirmed replica set",
                  "connectionString"_attr = connStr);
            Grid::get(_serviceContext)->shardRegistry()->updateReplSetHosts(connStr);
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
            LOGV2(471692, "Unable to update the shard registry", "error"_attr = e);
        }

        Grid::get(_serviceContext)
            ->getExecutorPool()
            ->getFixedExecutor()
            ->schedule([serviceContext = _serviceContext, connStr](Status status) {
                if (ErrorCodes::isCancelationError(status.code())) {
                    LOGV2_DEBUG(22067,
                                2,
                                "Unable to schedule confirmed replica set update due to {error}",
                                "Unable to schedule confirmed replica set update",
                                "error"_attr = status);
                    return;
                }
                invariant(status);

                try {
                    LOGV2(22068,
                          "Updating config server with confirmed replica set {connectionString}",
                          "Updating config server with confirmed replica set",
                          "connectionString"_attr = connStr);

                    if (MONGO_unlikely(failUpdateShardIdentityConfigString.shouldFail())) {
                        return;
                    }

                    auto configsvrConnStr = Grid::get(serviceContext)
                                                ->shardRegistry()
                                                ->getConfigServerConnectionString();

                    // Only proceed if the notification is for the configsvr
                    if (configsvrConnStr.getSetName() != connStr.getSetName()) {
                        return;
                    }

                    ThreadClient tc("updateShardIdentityConfigString", serviceContext);
                    auto opCtx = tc->makeOperationContext();

                    ShardingInitializationMongoD::updateShardIdentityConfigString(opCtx.get(),
                                                                                  connStr);
                } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
                    LOGV2(22069, "Unable to update config server", "error"_attr = e);
                }
            });
    }
    void onPossibleSet(const State& state) noexcept final {
        try {
            Grid::get(_serviceContext)->shardRegistry()->updateReplSetHosts(state.connStr);
        } catch (const DBException& ex) {
            LOGV2_DEBUG(22070,
                        2,
                        "Unable to update config server with possible replica set",
                        "error"_attr = ex);
        }
    }
    void onDroppedSet(const Key&) noexcept final {}

private:
    ServiceContext* _serviceContext;
};

}  // namespace

void ShardingInitializationMongoD::initializeShardingEnvironmentOnShardServer(
    OperationContext* opCtx, const ShardIdentity& shardIdentity, StringData distLockProcessId) {
    initializeGlobalShardingStateForMongoD(
        opCtx, shardIdentity.getConfigsvrConnectionString(), distLockProcessId);

    _replicaSetChangeListener =
        ReplicaSetMonitor::getNotifier().makeListener<ShardingReplicaSetChangeListener>(
            opCtx->getServiceContext());

    // Determine primary/secondary/standalone state in order to properly initialize sharding
    // components.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    bool isStandaloneOrPrimary =
        !isReplSet || (replCoord->getMemberState() == repl::MemberState::RS_PRIMARY);

    getCatalogCacheLoaderForFiltering(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);
    ChunkSplitter::get(opCtx).onShardingInitialization(isStandaloneOrPrimary);
    PeriodicBalancerConfigRefresher::get(opCtx).onShardingInitialization(opCtx->getServiceContext(),
                                                                         isStandaloneOrPrimary);

    // Start the transaction coordinator service only if the node is the primary of a replica set
    TransactionCoordinatorService::get(opCtx)->onShardingInitialization(
        opCtx, isReplSet && isStandaloneOrPrimary);

    Grid::get(opCtx)->setShardingInitialized();

    LOGV2(22071,
          "Finished initializing sharding components for {memberState} node.",
          "Finished initializing sharding components",
          "memberState"_attr = (isStandaloneOrPrimary ? "primary" : "secondary"));
}

ShardingInitializationMongoD::ShardingInitializationMongoD()
    : _initFunc([this](auto... args) {
          this->initializeShardingEnvironmentOnShardServer(std::forward<decltype(args)>(args)...);
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
    auto const grid = Grid::get(opCtx);

    if (!shardingState->enabled())
        return;

    grid->getExecutorPool()->shutdownAndJoin();
    grid->catalogClient()->shutDown(opCtx);
    grid->shardRegistry()->shutdown();
    _replicaSetChangeListener.reset();
}

bool ShardingInitializationMongoD::initializeShardingAwarenessIfNeeded(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    // In sharded readOnly mode, we ignore the shardIdentity document on disk and instead *require*
    // a shardIdentity document to be passed through --overrideShardIdentity
    if (storageGlobalParams.readOnly) {
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            uassert(ErrorCodes::InvalidOptions,
                    "If started with --shardsvr in queryableBackupMode, a shardIdentity document "
                    "must be provided through --overrideShardIdentity",
                    !serverGlobalParams.overrideShardIdentity.isEmpty());

            auto overrideShardIdentity =
                uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(
                    serverGlobalParams.overrideShardIdentity));

            {
                // Global lock is required to call initializeFromShardIdentity
                Lock::GlobalWrite lk(opCtx);
                initializeFromShardIdentity(opCtx, overrideShardIdentity);
            }

            return true;
        } else {
            // Error if --overrideShardIdentity is used but *not* started with --shardsvr
            uassert(ErrorCodes::InvalidOptions,
                    str::stream()
                        << "Not started with --shardsvr, but a shardIdentity document was provided "
                           "through --overrideShardIdentity: "
                        << serverGlobalParams.overrideShardIdentity,
                    serverGlobalParams.overrideShardIdentity.isEmpty());
            return false;
        }

        MONGO_UNREACHABLE;
    }

    // In sharded *non*-readOnly mode, error if --overrideShardIdentity is provided
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "--overrideShardIdentity is only allowed in sharded "
                             "queryableBackupMode. If not in queryableBackupMode, you can edit "
                             "the shardIdentity document by starting the server *without* "
                             "--shardsvr, manually updating the shardIdentity document in the "
                          << NamespaceString::kServerConfigurationNamespace.toString()
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

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        if (!foundShardIdentity) {
            LOGV2_WARNING(22074,
                          "Started with --shardsvr, but no shardIdentity document was found on "
                          "disk in {namespace}. This most "
                          "likely means this server has not yet been added to a "
                          "sharded cluster.",
                          "Started with --shardsvr, but no shardIdentity document was found on "
                          "disk. This most likely means this server has not yet been added to a "
                          "sharded cluster",
                          "namespace"_attr = NamespaceString::kServerConfigurationNamespace);
            return false;
        }

        invariant(!shardIdentityBSON.isEmpty());

        auto shardIdentity =
            uassertStatusOK(ShardIdentityType::fromShardIdentityDocument(shardIdentityBSON));

        {
            // Global lock is required to call initializeFromShardIdentity
            Lock::GlobalWrite lk(opCtx);
            initializeFromShardIdentity(opCtx, shardIdentity);
        }

        return true;
    } else {
        // Warn if a shardIdentity document is found on disk but *not* started with --shardsvr.
        if (!shardIdentityBSON.isEmpty()) {
            LOGV2_WARNING(
                22075,
                "Not started with --shardsvr, but a shardIdentity document was found "
                "on disk in {namespace}: {shardIdentityDocument}",
                "Not started with --shardsvr, but a shardIdentity document was found on disk",
                "namespace"_attr = NamespaceString::kServerConfigurationNamespace,
                "shardIdentityDocument"_attr = shardIdentityBSON);
        }
        return false;
    }
}

void ShardingInitializationMongoD::initializeFromShardIdentity(
    OperationContext* opCtx, const ShardIdentityType& shardIdentity) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
    invariant(opCtx->lockState()->isLocked());

    uassertStatusOKWithContext(
        shardIdentity.validate(),
        "Invalid shard identity document found when initializing sharding state");

    LOGV2(22072,
          "Initializing sharding state with: {initialShardIdentity}",
          "Initializing sharding state",
          "initialShardIdentity"_attr = shardIdentity);

    const auto& configSvrConnStr = shardIdentity.getConfigsvrConnectionString();

    auto const shardingState = ShardingState::get(opCtx);
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

    stdx::unique_lock<Latch> ul(_initSynchronizationMutex);

    if (shardingState->enabled()) {
        uassert(40371, "", shardingState->shardId() == shardIdentity.getShardName());
        uassert(40372, "", shardingState->clusterId() == shardIdentity.getClusterId());

        auto prevConfigsvrConnStr = shardRegistry->getConfigServerConnectionString();
        uassert(40373, "", prevConfigsvrConnStr.type() == ConnectionString::SET);
        uassert(40374, "", prevConfigsvrConnStr.getSetName() == configSvrConnStr.getSetName());

        return;
    }

    auto initializationStatus = shardingState->initializationStatus();
    uassert(ErrorCodes::ManualInterventionRequired,
            str::stream() << "Server's sharding metadata manager failed to initialize and will "
                             "remain in this state until the instance is manually reset"
                          << causedBy(*initializationStatus),
            !initializationStatus);

    try {
        _initFunc(opCtx, shardIdentity, generateDistLockProcessId(opCtx));
        shardingState->setInitialized(shardIdentity.getShardName().toString(),
                                      shardIdentity.getClusterId());
    } catch (const DBException& ex) {
        shardingState->setInitialized(ex.toStatus());
    }
}

void ShardingInitializationMongoD::updateShardIdentityConfigString(
    OperationContext* opCtx, const ConnectionString& newConnectionString) {
    BSONObj updateObj(
        ShardIdentityType::createConfigServerUpdateObject(newConnectionString.toString()));

    auto updateReq = UpdateRequest();
    updateReq.setNamespaceString(NamespaceString::kServerConfigurationNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdateModification(updateObj);

    try {
        AutoGetOrCreateDb autoDb(
            opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_X);

        auto result = update(opCtx, autoDb.getDb(), updateReq);
        if (result.numMatched == 0) {
            LOGV2_WARNING(22076,
                          "Failed to update config server connection string of shard identity "
                          "document because it does not exist. This shard could have been removed "
                          "from the cluster");
        } else {
            LOGV2_DEBUG(22073,
                        2,
                        "Updated config server connection string in shardIdentity document "
                        "to {newConnectionString}",
                        "Updated config server connection string in shardIdentity document",
                        "newConnectionString"_attr = newConnectionString);
        }
    } catch (const DBException& exception) {
        auto status = exception.toStatus();
        if (!ErrorCodes::isNotMasterError(status.code())) {
            LOGV2_WARNING(22077,
                          "Error encountered while trying to update config connection string to "
                          "{newConnectionString} {error}",
                          "Error encountered while trying to update config connection string",
                          "newConnectionString"_attr = newConnectionString.toString(),
                          "error"_attr = redact(status));
        }
    }
}

void initializeGlobalShardingStateForMongoD(OperationContext* opCtx,
                                            const ConnectionString& configCS,
                                            StringData distLockProcessId) {
    auto targeterFactory = std::make_unique<RemoteCommandTargeterFactoryImpl>();
    auto targeterFactoryPtr = targeterFactory.get();

    ShardFactory::BuilderCallable setBuilder = [targeterFactoryPtr](
                                                   const ShardId& shardId,
                                                   const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuilderCallable masterBuilder = [targeterFactoryPtr](
                                                      const ShardId& shardId,
                                                      const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuilderCallable localBuilder = [](const ShardId& shardId,
                                                    const ConnectionString& connStr) {
        return std::make_unique<ShardLocal>(shardId);
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
        {ConnectionString::LOCAL, std::move(localBuilder)},
    };

    auto shardFactory =
        std::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto const service = opCtx->getServiceContext();


    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        hasAdditionalCatalogCacheForFiltering()) {
        // Setup additional CatalogCache for filtering only
        setCatalogCacheLoaderForFiltering(service,
                                          std::make_unique<ShardServerCatalogCacheLoader>(
                                              std::make_unique<ConfigServerCatalogCacheLoader>()));
        setCatalogCacheForFiltering(
            service, std::make_unique<CatalogCache>(getCatalogCacheLoaderForFiltering(opCtx)));
        registerShutdownTask(
            [service]() { getCatalogCacheLoaderForFiltering(service).shutDown(); });
    }

    // Make primary CatalogCacheLoader according to the cluster Role
    auto makeCatalogCacheLoader = []() -> std::unique_ptr<CatalogCacheLoader> {
        switch (serverGlobalParams.clusterRole) {
            case ClusterRole::ShardServer:
                if (storageGlobalParams.readOnly) {
                    return std::make_unique<ReadOnlyCatalogCacheLoader>();
                }
                if (hasAdditionalCatalogCacheForFiltering()) {
                    // The primary CatalogCache will be used only for routing
                    return std::make_unique<ConfigServerCatalogCacheLoader>();
                }

                // Normal ShardServer without additional cache for filtering
                return std::make_unique<ShardServerCatalogCacheLoader>(
                    std::make_unique<ConfigServerCatalogCacheLoader>());
                break;

            case ClusterRole::ConfigServer:
                return std::make_unique<ConfigServerCatalogCacheLoader>();
                break;

            default:
                MONGO_UNREACHABLE;
        }
    };

    CatalogCacheLoader::set(service, makeCatalogCacheLoader());

    auto validator = LogicalTimeValidator::get(service);
    if (validator) {  // The keyManager may be existing if the node was a part of a standalone RS.
        validator->stopKeyManager();
    }

    globalConnPool.addHook(new ShardingConnectionHook(makeEgressHooksList(service)));

    auto catalogCache = std::make_unique<CatalogCache>(CatalogCacheLoader::get(opCtx));

    // List of hooks which will be called by the ShardRegistry when it discovers a shard has been
    // removed.
    std::vector<ShardRegistry::ShardRemovalHook> shardRemovalHooks = {
        // Invalidate appropriate entries in the CatalogCache when a shard is removed. It's safe to
        // capture the CatalogCache pointer since the Grid (and therefore CatalogCache and
        // ShardRegistry) are never destroyed.
        [catCache = catalogCache.get()](const ShardId& removedShard) {
            catCache->invalidateEntriesThatReferenceShard(removedShard);
        }};

    uassert(ErrorCodes::BadValue,
            "Unrecognized connection string.",
            configCS.type() != ConnectionString::INVALID);

    auto shardRegistry = std::make_unique<ShardRegistry>(
        std::move(shardFactory), configCS, std::move(shardRemovalHooks));

    uassertStatusOK(
        initializeGlobalShardingState(opCtx,
                                      distLockProcessId,
                                      std::move(catalogCache),
                                      std::move(shardRegistry),
                                      [service] { return makeEgressHooksList(service); },
                                      // We only need one task executor here because sharding task
                                      // executors aren't used for user queries in mongod.
                                      1));

    auto const replCoord = repl::ReplicationCoordinator::get(service);
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        replCoord->getMemberState().primary()) {
        LogicalTimeValidator::get(opCtx)->enableKeyGenerator(opCtx, true);
    }
}

}  // namespace mongo
