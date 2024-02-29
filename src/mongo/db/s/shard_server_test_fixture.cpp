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

#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ShardServerTestFixture::ShardServerTestFixture(Options options, bool setUpMajorityReads)
    : ShardingMongoDTestFixture(std::move(options), setUpMajorityReads) {}

ShardServerTestFixture::~ShardServerTestFixture() = default;

std::shared_ptr<RemoteCommandTargeterMock> ShardServerTestFixture::configTargeterMock() {
    return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
}

void ShardServerTestFixture::setUp() {
    ShardingMongoDTestFixture::setUp();

    replicationCoordinator()->alwaysAllowWrites(true);

    ShardingState::get(getServiceContext())
        ->setRecoveryCompleted({OID::gen(),
                                ClusterRole::ShardServer,
                                ConnectionString(kConfigHostAndPort),
                                kMyShardName});

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

std::unique_ptr<ShardingCatalogClient> ShardServerTestFixture::makeShardingCatalogClient() {
    return std::make_unique<ShardingCatalogClientImpl>(nullptr /* overrideConfigShard */);
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

CatalogCacheLoaderMock* ShardServerTestFixtureWithCatalogCacheMock::getCatalogCacheLoaderMock() {
    return _cacheLoaderMock;
}

void ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp() {
    auto loader = std::make_unique<CatalogCacheLoaderMock>();
    _cacheLoaderMock = loader.get();
    setCatalogCacheLoader(std::move(loader));
    ShardServerTestFixture::setUp();
}

CatalogCacheMock* ShardServerTestFixtureWithCatalogCacheLoaderMock::getCatalogCacheMock() {
    return static_cast<CatalogCacheMock*>(catalogCache());
}

CatalogCacheLoaderMock*
ShardServerTestFixtureWithCatalogCacheLoaderMock::getCatalogCacheLoaderMock() {
    return _cacheLoaderMock;
}

}  // namespace mongo
