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

#include "mongo/db/sharding_environment/shard_server_test_fixture.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader_impl.h"
#include "mongo/db/global_catalog/sharding_catalog_client_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

ShardServerTestFixture::ShardServerTestFixture(Options options, bool setUpMajorityReads)
    : ShardingMongoDTestFixture(std::move(options), setUpMajorityReads) {}

ShardServerTestFixture::~ShardServerTestFixture() = default;

std::shared_ptr<RemoteCommandTargeterMock> ShardServerTestFixture::configTargeterMock() {
    return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
}

void ShardServerTestFixture::setUp() {
    ShardingMongoDTestFixture::setUp();

    ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));

    ShardingState::get(getServiceContext())
        ->setRecoveryCompleted({OID::gen(),
                                ClusterRole::ShardServer,
                                ConnectionString(kConfigHostAndPort),
                                kMyShardName});

    if (!_configServerCatalogCacheLoader) {
        _configServerCatalogCacheLoader = std::make_shared<ConfigServerCatalogCacheLoaderImpl>();
    }

    if (!_shardServerCatalogCacheLoader) {
        _shardServerCatalogCacheLoader = std::make_shared<ShardServerCatalogCacheLoaderImpl>(
            std::make_unique<ConfigServerCatalogCacheLoaderImpl>());
    }

    if (!_catalogCache) {
        _catalogCache =
            std::make_unique<CatalogCache>(getServiceContext(),
                                           _configServerCatalogCacheLoader,
                                           _shardServerCatalogCacheLoader,
                                           true /* cascadeDatabaseCacheLoaderShutdown */,
                                           false /* cascadeCollectionCacheLoaderShutdown */);
    }

    uassertStatusOK(
        initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort),
                                                      std::move(_catalogCache),
                                                      _shardServerCatalogCacheLoader));

    // Set the findHost() return value on the mock targeter so that later calls to the
    // config targeter's findHost() return the appropriate value.
    configTargeterMock()->setFindHostReturnValue(kConfigHostAndPort);
}

void ShardServerTestFixture::setConfigServerCatalogCacheLoader(
    std::shared_ptr<ConfigServerCatalogCacheLoader> loader) {
    invariant(loader && !_catalogCache && !_configServerCatalogCacheLoader);
    _configServerCatalogCacheLoader = loader;
}

void ShardServerTestFixture::setShardServerCatalogCacheLoader(
    std::shared_ptr<ShardServerCatalogCacheLoader> loader) {
    invariant(loader && !_shardServerCatalogCacheLoader);
    _shardServerCatalogCacheLoader = loader;
}

void ShardServerTestFixture::setCatalogCache(std::unique_ptr<CatalogCache> cache) {
    invariant(cache && !_catalogCache);
    _catalogCache = std::move(cache);
}

std::unique_ptr<ShardingCatalogClient> ShardServerTestFixture::makeShardingCatalogClient() {
    return std::make_unique<ShardingCatalogClientImpl>(nullptr /* overrideConfigShard */);
}

void ShardServerTestFixtureWithCatalogCacheMock::setUp() {
    auto loader = std::make_shared<ShardServerCatalogCacheLoaderMock>();
    setShardServerCatalogCacheLoader(loader);
    setCatalogCache(std::make_unique<CatalogCacheMock>(getServiceContext(), std::move(loader)));
    ShardServerTestFixture::setUp();
}

CatalogCacheMock* ShardServerTestFixtureWithCatalogCacheMock::getCatalogCacheMock() {
    return static_cast<CatalogCacheMock*>(catalogCache());
}

std::shared_ptr<ShardServerCatalogCacheLoaderMock>
ShardServerTestFixtureWithCatalogCacheMock::getCatalogCacheLoaderMock() {
    auto mockLoader = std::dynamic_pointer_cast<ShardServerCatalogCacheLoaderMock>(
        _shardServerCatalogCacheLoader);
    invariant(mockLoader);
    return mockLoader;
}

void ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp() {
    auto configServerLoader = std::make_shared<ConfigServerCatalogCacheLoaderMock>();
    setConfigServerCatalogCacheLoader(std::move(configServerLoader));

    auto shardServerLoader = std::make_shared<ShardServerCatalogCacheLoaderMock>();
    setShardServerCatalogCacheLoader(std::move(shardServerLoader));

    ShardServerTestFixture::setUp();
}

CatalogCacheMock* ShardServerTestFixtureWithCatalogCacheLoaderMock::getCatalogCacheMock() {
    return static_cast<CatalogCacheMock*>(catalogCache());
}

std::shared_ptr<ConfigServerCatalogCacheLoaderMock>
ShardServerTestFixtureWithCatalogCacheLoaderMock::getConfigServerCatalogCacheLoaderMock() {
    auto mockLoader = std::dynamic_pointer_cast<ConfigServerCatalogCacheLoaderMock>(
        _configServerCatalogCacheLoader);
    invariant(mockLoader);
    return mockLoader;
}

std::shared_ptr<ShardServerCatalogCacheLoaderMock>
ShardServerTestFixtureWithCatalogCacheLoaderMock::getCatalogCacheLoaderMock() {
    auto mockLoader = std::dynamic_pointer_cast<ShardServerCatalogCacheLoaderMock>(
        _shardServerCatalogCacheLoader);
    invariant(mockLoader);
    return mockLoader;
}

}  // namespace mongo
