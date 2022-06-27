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

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_test_fixture.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/config_server_catalog_cache_loader.h"

namespace mongo {

const HostAndPort ShardServerTestFixture::kConfigHostAndPort("dummy", 123);

ShardServerTestFixture::ShardServerTestFixture(Options options, bool setUpMajorityReads)
    : ShardingMongodTestFixture(std::move(options), setUpMajorityReads) {}

ShardServerTestFixture::~ShardServerTestFixture() = default;

std::shared_ptr<RemoteCommandTargeterMock> ShardServerTestFixture::configTargeterMock() {
    return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
}

void ShardServerTestFixture::setUp() {
    ShardingMongodTestFixture::setUp();

    replicationCoordinator()->alwaysAllowWrites(true);

    // Initialize sharding components as a shard server.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    _clusterId = OID::gen();
    ShardingState::get(getServiceContext())->setInitialized(_myShardName, _clusterId);

    if (!_catalogCacheLoader)
        _catalogCacheLoader = std::make_unique<ShardServerCatalogCacheLoader>(
            std::make_unique<ConfigServerCatalogCacheLoader>());
    CatalogCacheLoader::set(getServiceContext(), std::move(_catalogCacheLoader));

    uassertStatusOK(
        initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort)));

    // Set the findHost() return value on the mock targeter so that later calls to the
    // config targeter's findHost() return the appropriate value.
    configTargeterMock()->setFindHostReturnValue(kConfigHostAndPort);
}

// Sets the catalog cache loader for mocking. This must be called before the setUp function is
// invoked.
void ShardServerTestFixture::setCatalogCacheLoader(std::unique_ptr<CatalogCacheLoader> loader) {
    invariant(loader && !_catalogCacheLoader);
    _catalogCacheLoader = std::move(loader);
}

void ShardServerTestFixture::tearDown() {
    CatalogCacheLoader::clearForTests(getServiceContext());

    ShardingMongodTestFixture::tearDown();
}

std::unique_ptr<ShardingCatalogClient> ShardServerTestFixture::makeShardingCatalogClient() {
    return std::make_unique<ShardingCatalogClientImpl>();
}

void ShardServerTestFixtureWithCatalogCacheMock::setUp() {
    auto loader = std::make_unique<CatalogCacheLoaderMock>();
    _cacheLoaderMock = loader.get();
    setCatalogCacheLoader(std::move(loader));
    ShardServerTestFixture::setUp();
}

std::unique_ptr<CatalogCache> ShardServerTestFixtureWithCatalogCacheMock::makeCatalogCache() {
    return std::make_unique<CatalogCacheMock>(getServiceContext(), *_cacheLoaderMock);
}

CatalogCacheMock* ShardServerTestFixtureWithCatalogCacheMock::getCatalogCacheMock() {
    return static_cast<CatalogCacheMock*>(catalogCache());
}

}  // namespace mongo
