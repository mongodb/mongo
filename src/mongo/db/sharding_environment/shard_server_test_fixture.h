// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_mock.h"
#include "mongo/db/router_role/routing_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/router_role/routing_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/router_role/routing_cache/shard_server_catalog_cache_loader_mock.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Test fixture for shard components, as opposed to config or mongos components. Provides a mock
 * network via ShardingMongoDTestFixture.
 */
class [[MONGO_MOD_OPEN]] ShardServerTestFixture : public ShardingMongoDTestFixture {
protected:
    ShardServerTestFixture(Options options = {}, bool setUpMajorityReads = true);
    ~ShardServerTestFixture() override;

    void setUp() override;

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override;

    /**
     * Sets the config server catalog cache loader for mocking. This must be called before the setUp
     * function is invoked.
     */
    void setConfigServerCatalogCacheLoader(std::shared_ptr<ConfigServerCatalogCacheLoader> loader);

    /**
     * Sets the shard server catalog cache loader for mocking. This must be called before the setUp
     * function is invoked.
     */
    void setShardServerCatalogCacheLoader(std::shared_ptr<ShardServerCatalogCacheLoader> loader);

    /**
     * Sets the catalog cache for mocking. This must be called before the setUp function is invoked.
     */
    void setCatalogCache(std::unique_ptr<CatalogCache> cache);

    /**
     * Returns the mock targeter for the config server. Useful to use like so,
     *
     *     configTargeterMock()->setFindHostReturnValue(HostAndPort);
     *     configTargeterMock()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"})
     *
     * Remote calls always need to resolve a host with RemoteCommandTargeterMock::findHost, so it
     * must be set.
     */
    std::shared_ptr<RemoteCommandTargeterMock> configTargeterMock();

    const HostAndPort kConfigHostAndPort{"dummy", 123};
    ShardId kMyShardName{"myShardName"};

    service_context_test::ShardRoleOverride _shardRole;

    std::shared_ptr<ConfigServerCatalogCacheLoader> _configServerCatalogCacheLoader;
    std::shared_ptr<ShardServerCatalogCacheLoader> _shardServerCatalogCacheLoader;
    std::unique_ptr<CatalogCache> _catalogCache;
};

class [[MONGO_MOD_OPEN]] ShardServerTestFixtureWithCatalogCacheMock
    : public ShardServerTestFixture {
public:
    ShardServerTestFixtureWithCatalogCacheMock() : ShardServerTestFixture() {}
    ShardServerTestFixtureWithCatalogCacheMock(Options options)
        : ShardServerTestFixture(std::move(options)) {}

protected:
    void setUp() override;
    CatalogCacheMock* getCatalogCacheMock();
    std::shared_ptr<ConfigServerCatalogCacheLoaderMock> getConfigServerCatalogCacheLoaderMock();
};

class [[MONGO_MOD_OPEN]] ShardServerTestFixtureWithCatalogCacheLoaderMock
    : public ShardServerTestFixture {
protected:
    void setUp() override;
    CatalogCacheMock* getCatalogCacheMock();
    std::shared_ptr<ConfigServerCatalogCacheLoaderMock> getConfigServerCatalogCacheLoaderMock();
    std::shared_ptr<ShardServerCatalogCacheLoaderMock> getShardServerCatalogCacheLoaderMock();
};

}  // namespace mongo
