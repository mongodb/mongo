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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_catalog_manager.h"

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

// This value is initialized only if the node is running as a config server
const auto getShardingCatalogManager =
    ServiceContext::declareDecoration<boost::optional<ShardingCatalogManager>>();

}  // namespace

void ShardingCatalogManager::create(ServiceContext* serviceContext,
                                    std::unique_ptr<executor::TaskExecutor> addShardExecutor) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(!shardingCatalogManager);

    shardingCatalogManager.emplace(serviceContext, std::move(addShardExecutor));
}

void ShardingCatalogManager::clearForTests(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    shardingCatalogManager.reset();
}

ShardingCatalogManager* ShardingCatalogManager::get(ServiceContext* serviceContext) {
    auto& shardingCatalogManager = getShardingCatalogManager(serviceContext);
    invariant(shardingCatalogManager);

    return shardingCatalogManager.get_ptr();
}

ShardingCatalogManager* ShardingCatalogManager::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

ShardingCatalogManager::ShardingCatalogManager(
    ServiceContext* serviceContext, std::unique_ptr<executor::TaskExecutor> addShardExecutor)
    : _serviceContext(serviceContext),
      _executorForAddShard(std::move(addShardExecutor)),
      _kZoneOpLock("zoneOpLock"),
      _kChunkOpLock("chunkOpLock"),
      _kShardMembershipLock("shardMembershipLock") {
    startup();
}

ShardingCatalogManager::~ShardingCatalogManager() {
    shutDown();
}

void ShardingCatalogManager::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return;
    }
    _started = true;
    _executorForAddShard->startup();

    Grid::get(_serviceContext)
        ->setCustomConnectionPoolStatsFn(
            [this](executor::ConnectionPoolStats* stats) { appendConnectionStats(stats); });
}

void ShardingCatalogManager::shutDown() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    Grid::get(_serviceContext)->setCustomConnectionPoolStatsFn(nullptr);

    _executorForAddShard->shutdown();
    _executorForAddShard->join();
}

Status ShardingCatalogManager::initializeConfigDatabaseIfNeeded(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_configInitialized) {
            return {ErrorCodes::AlreadyInitialized,
                    "Config database was previously loaded into memory"};
        }
    }

    Status status = _initConfigIndexes(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Make sure to write config.version last since we detect rollbacks of config.version and
    // will re-run initializeConfigDatabaseIfNeeded if that happens, but we don't detect rollback
    // of the index builds.
    status = _initConfigVersion(opCtx);
    if (!status.isOK()) {
        return status;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = true;

    return Status::OK();
}

void ShardingCatalogManager::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = false;
}

Status ShardingCatalogManager::_initConfigVersion(OperationContext* opCtx) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    auto versionStatus =
        catalogClient->getConfigVersion(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!versionStatus.isOK()) {
        return versionStatus.getStatus();
    }

    const auto& versionInfo = versionStatus.getValue();
    if (versionInfo.getMinCompatibleVersion() > CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "current version v" << CURRENT_CONFIG_VERSION
                              << " is older than the cluster min compatible v"
                              << versionInfo.getMinCompatibleVersion()};
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_EmptyVersion) {
        VersionType newVersion;
        newVersion.setClusterId(OID::gen());
        newVersion.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
        newVersion.setCurrentVersion(CURRENT_CONFIG_VERSION);

        BSONObj versionObj(newVersion.toBSON());
        auto insertStatus = catalogClient->insertConfigDocument(
            opCtx, VersionType::ConfigNS, versionObj, kNoWaitWriteConcern);

        return insertStatus;
    }

    if (versionInfo.getCurrentVersion() == UpgradeHistory_UnreportedVersion) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                "Assuming config data is old since the version document cannot be found in the "
                "config server and it contains databases besides 'local' and 'admin'. "
                "Please upgrade if this is the case. Otherwise, make sure that the config "
                "server is clean."};
    }

    if (versionInfo.getCurrentVersion() < CURRENT_CONFIG_VERSION) {
        return {ErrorCodes::IncompatibleShardingConfigVersion,
                str::stream() << "need to upgrade current cluster version to v"
                              << CURRENT_CONFIG_VERSION
                              << "; currently at v"
                              << versionInfo.getCurrentVersion()};
    }

    return Status::OK();
}

Status ShardingCatalogManager::_initConfigIndexes(OperationContext* opCtx) {
    const bool unique = true;
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    Status result =
        configShard->createIndexOnConfig(opCtx,
                                         NamespaceString(ChunkType::ConfigNS),
                                         BSON(ChunkType::ns() << 1 << ChunkType::min() << 1),
                                         unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        NamespaceString(ChunkType::ConfigNS),
        BSON(ChunkType::ns() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_shard_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         NamespaceString(ChunkType::ConfigNS),
                                         BSON(ChunkType::ns() << 1 << ChunkType::lastmod() << 1),
                                         unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_lastmod_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        opCtx,
        NamespaceString(MigrationType::ConfigNS),
        BSON(MigrationType::ns() << 1 << MigrationType::min() << 1),
        unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config.migrations"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        opCtx, NamespaceString(ShardType::ConfigNS), BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create host_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        opCtx, NamespaceString(LocksType::ConfigNS), BSON(LocksType::lockID() << 1), !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create lock id index on config db"
                                    << causedBy(result));
    }

    result =
        configShard->createIndexOnConfig(opCtx,
                                         NamespaceString(LocksType::ConfigNS),
                                         BSON(LocksType::state() << 1 << LocksType::process() << 1),
                                         !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create state and process id index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        opCtx, NamespaceString(LockpingsType::ConfigNS), BSON(LockpingsType::ping() << 1), !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create lockping ping time index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(opCtx,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns() << 1 << TagsType::min() << 1),
                                              unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(opCtx,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns() << 1 << TagsType::tag() << 1),
                                              !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_tag_1 index on config db"
                                    << causedBy(result));
    }

    return Status::OK();
}

Status ShardingCatalogManager::setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                                      const BSONObj& cmdObj) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(opCtx->lockState(), _kShardMembershipLock);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern));

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        auto response = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            cmdObj,
            Shard::RetryPolicy::kIdempotent);
        if (!response.isOK()) {
            return response.getStatus();
        }
        if (!response.getValue().commandStatus.isOK()) {
            return response.getValue().commandStatus;
        }
        if (!response.getValue().writeConcernStatus.isOK()) {
            return response.getValue().writeConcernStatus;
        }
    }

    return Status::OK();
}

}  // namespace mongo
