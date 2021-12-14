
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_initialization_mongod.h"

#include "mongo/base/status.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/read_only_catalog_cache_loader.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/sharding_config_optime_gossip.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_local.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto getInstance = ServiceContext::declareDecoration<ShardingInitializationMongoD>();

auto makeEgressHooksList(ServiceContext* service) {
    auto unshardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    unshardedHookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(service));
    unshardedHookList->addHook(
        stdx::make_unique<rpc::ShardingEgressMetadataHookForMongod>(service));

    return unshardedHookList;
}

/**
 * Updates the config server field of the shardIdentity document with the given connection string if
 * setName is equal to the config server replica set name.
 *
 * NOTE: This is intended to be used on a new thread that hasn't called Client::initThread.
 *
 * One example use case is for the ReplicaSetMonitor asynchronous callback when it detects changes
 * to replica set membership.
 */
void updateShardIdentityConfigStringCB(const std::string& setName,
                                       const std::string& newConnectionString) {
    auto configsvrConnStr =
        Grid::get(getGlobalServiceContext())->shardRegistry()->getConfigServerConnectionString();
    if (configsvrConnStr.getSetName() != setName) {
        // Ignore all change notification for other sets that are not the config server.
        return;
    }

    Client::initThread("updateShardIdentityConfigConnString");
    auto uniqOpCtx = Client::getCurrent()->makeOperationContext();

    auto status = ShardingInitializationMongoD::updateShardIdentityConfigString(
        uniqOpCtx.get(), uassertStatusOK(ConnectionString::parse(newConnectionString)));
    if (!status.isOK() && !ErrorCodes::isNotMasterError(status.code())) {
        warning() << "Error encountered while trying to update config connection string to "
                  << newConnectionString << causedBy(redact(status));
    }
}

void initializeShardingEnvironmentOnShardServer(OperationContext* opCtx,
                                                const ShardIdentity& shardIdentity,
                                                StringData distLockProcessId) {
    initializeGlobalShardingStateForMongoD(
        opCtx, shardIdentity.getConfigsvrConnectionString(), distLockProcessId);

    ReplicaSetMonitor::setSynchronousConfigChangeHook(
        &ShardRegistry::replicaSetChangeShardRegistryUpdateHook);
    ReplicaSetMonitor::setAsynchronousConfigChangeHook(&updateShardIdentityConfigStringCB);

    // Determine primary/secondary/standalone state in order to properly initialize sharding
    // components.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    bool isStandaloneOrPrimary =
        !isReplSet || (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                       repl::MemberState::RS_PRIMARY);

    CatalogCacheLoader::get(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);

    Grid::get(opCtx)->setShardingInitialized();

    LOG(0) << "Finished initializing sharding components for "
           << (isStandaloneOrPrimary ? "primary" : "secondary") << " node.";
}

}  // namespace

ShardingInitializationMongoD::ShardingInitializationMongoD()
    : _initFunc(initializeShardingEnvironmentOnShardServer) {}

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

    grid->catalogClient()->shutDown(opCtx);
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
            warning() << "Started with --shardsvr, but no shardIdentity document was found on "
                         "disk in "
                      << NamespaceString::kServerConfigurationNamespace
                      << ". This most likely means this server has not yet been added to a "
                         "sharded cluster.";
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
            warning() << "Not started with --shardsvr, but a shardIdentity document was found "
                         "on disk in "
                      << NamespaceString::kServerConfigurationNamespace << ": "
                      << shardIdentityBSON;
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

    log() << "initializing sharding state with: " << shardIdentity;

    const auto& configSvrConnStr = shardIdentity.getConfigsvrConnectionString();

    auto const shardingState = ShardingState::get(opCtx);
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

    stdx::unique_lock<stdx::mutex> ul(_initSynchronizationMutex);

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

Status ShardingInitializationMongoD::updateShardIdentityConfigString(
    OperationContext* opCtx, const ConnectionString& newConnectionString) {
    BSONObj updateObj(
        ShardIdentityType::createConfigServerUpdateObject(newConnectionString.toString()));

    UpdateRequest updateReq(NamespaceString::kServerConfigurationNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdates(updateObj);
    UpdateLifecycleImpl updateLifecycle(NamespaceString::kServerConfigurationNamespace);
    updateReq.setLifecycle(&updateLifecycle);

    try {
        AutoGetOrCreateDb autoDb(
            opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_X);

        auto result = update(opCtx, autoDb.getDb(), updateReq);
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

void initializeGlobalShardingStateForMongoD(OperationContext* opCtx,
                                            const ConnectionString& configCS,
                                            StringData distLockProcessId) {
    auto targeterFactory = stdx::make_unique<RemoteCommandTargeterFactoryImpl>();
    auto targeterFactoryPtr = targeterFactory.get();

    ShardFactory::BuilderCallable setBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable masterBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable localBuilder = [](const ShardId& shardId,
                                                    const ConnectionString& connStr) {
        return stdx::make_unique<ShardLocal>(shardId);
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
        {ConnectionString::LOCAL, std::move(localBuilder)},
    };

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto const service = opCtx->getServiceContext();

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        if (storageGlobalParams.readOnly) {
            CatalogCacheLoader::set(service, stdx::make_unique<ReadOnlyCatalogCacheLoader>());
        } else {
            CatalogCacheLoader::set(service,
                                    stdx::make_unique<ShardServerCatalogCacheLoader>(
                                        stdx::make_unique<ConfigServerCatalogCacheLoader>()));
        }
    } else {
        CatalogCacheLoader::set(service, stdx::make_unique<ConfigServerCatalogCacheLoader>());
    }

    auto validator = LogicalTimeValidator::get(service);
    if (validator) {  // The keyManager may be existing if the node was a part of a standalone RS.
        validator->stopKeyManager();
    }

    globalConnPool.addHook(new ShardingConnectionHook(false, makeEgressHooksList(service)));
    shardConnectionPool.addHook(new ShardingConnectionHook(true, makeEgressHooksList(service)));

    uassertStatusOK(initializeGlobalShardingState(
        opCtx,
        configCS,
        distLockProcessId,
        std::move(shardFactory),
        stdx::make_unique<CatalogCache>(CatalogCacheLoader::get(opCtx)),
        [service] { return makeEgressHooksList(service); },
        // We only need one task executor here because sharding task executors aren't used for user
        // queries in mongod.
        1));

    auto const replCoord = repl::ReplicationCoordinator::get(service);
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        replCoord->getMemberState().primary()) {
        LogicalTimeValidator::get(opCtx)->enableKeyGenerator(opCtx, true);
    }
}

}  // namespace mongo
