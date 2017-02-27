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

#include "mongo/s/catalog/sharding_catalog_manager_impl.h"

#include "mongo/base/status_with.h"
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

}  // namespace

ShardingCatalogManagerImpl::ShardingCatalogManagerImpl(
    std::unique_ptr<executor::TaskExecutor> addShardExecutor)
    : _executorForAddShard(std::move(addShardExecutor)),
      _kZoneOpLock("zoneOpLock"),
      _kChunkOpLock("chunkOpLock"),
      _kShardMembershipLock("shardMembershipLock") {}

ShardingCatalogManagerImpl::~ShardingCatalogManagerImpl() = default;

Status ShardingCatalogManagerImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return Status::OK();
    }
    _started = true;
    _executorForAddShard->startup();
    return Status::OK();
}

void ShardingCatalogManagerImpl::shutDown(OperationContext* txn) {
    LOG(1) << "ShardingCatalogManagerImpl::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    _executorForAddShard->shutdown();
    _executorForAddShard->join();
}

Status ShardingCatalogManagerImpl::initializeConfigDatabaseIfNeeded(OperationContext* txn) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_configInitialized) {
            return {ErrorCodes::AlreadyInitialized,
                    "Config database was previously loaded into memory"};
        }
    }

    Status status = _initConfigIndexes(txn);
    if (!status.isOK()) {
        return status;
    }

    // Make sure to write config.version last since we detect rollbacks of config.version and
    // will re-run initializeConfigDatabaseIfNeeded if that happens, but we don't detect rollback
    // of the index builds.
    status = _initConfigVersion(txn);
    if (!status.isOK()) {
        return status;
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = true;

    return Status::OK();
}

void ShardingCatalogManagerImpl::discardCachedConfigDatabaseInitializationState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _configInitialized = false;
}

Status ShardingCatalogManagerImpl::_initConfigVersion(OperationContext* txn) {
    const auto catalogClient = Grid::get(txn)->catalogClient(txn);

    auto versionStatus =
        catalogClient->getConfigVersion(txn, repl::ReadConcernLevel::kLocalReadConcern);
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
            txn, VersionType::ConfigNS, versionObj, kNoWaitWriteConcern);

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

Status ShardingCatalogManagerImpl::_initConfigIndexes(OperationContext* txn) {
    const bool unique = true;
    auto configShard = Grid::get(txn)->shardRegistry()->getConfigShard();

    Status result =
        configShard->createIndexOnConfig(txn,
                                         NamespaceString(ChunkType::ConfigNS),
                                         BSON(ChunkType::ns() << 1 << ChunkType::min() << 1),
                                         unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn,
        NamespaceString(ChunkType::ConfigNS),
        BSON(ChunkType::ns() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
        unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_shard_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn,
        NamespaceString(ChunkType::ConfigNS),
        BSON(ChunkType::ns() << 1 << ChunkType::DEPRECATED_lastmod() << 1),
        unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_lastmod_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn,
        NamespaceString(MigrationType::ConfigNS),
        BSON(MigrationType::ns() << 1 << MigrationType::min() << 1),
        unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config.migrations"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn, NamespaceString(ShardType::ConfigNS), BSON(ShardType::host() << 1), unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create host_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn, NamespaceString(LocksType::ConfigNS), BSON(LocksType::lockID() << 1), !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create lock id index on config db"
                                    << causedBy(result));
    }

    result =
        configShard->createIndexOnConfig(txn,
                                         NamespaceString(LocksType::ConfigNS),
                                         BSON(LocksType::state() << 1 << LocksType::process() << 1),
                                         !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create state and process id index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(
        txn, NamespaceString(LockpingsType::ConfigNS), BSON(LockpingsType::ping() << 1), !unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create lockping ping time index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(txn,
                                              NamespaceString(TagsType::ConfigNS),
                                              BSON(TagsType::ns() << 1 << TagsType::min() << 1),
                                              unique);
    if (!result.isOK()) {
        return Status(result.code(),
                      str::stream() << "couldn't create ns_1_min_1 index on config db"
                                    << causedBy(result));
    }

    result = configShard->createIndexOnConfig(txn,
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

Status ShardingCatalogManagerImpl::setFeatureCompatibilityVersionOnShards(
    OperationContext* txn, const std::string& version) {

    // No shards should be added until we have forwarded featureCompatibilityVersion to all shards.
    Lock::SharedLock lk(txn->lockState(), _kShardMembershipLock);

    std::vector<ShardId> shardIds;
    Grid::get(txn)->shardRegistry()->getAllShardIds(&shardIds);
    for (const ShardId& shardId : shardIds) {
        const auto shardStatus = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        auto response = shard->runCommandWithFixedRetryAttempts(
            txn,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON(FeatureCompatibilityVersion::kCommandName << version),
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
