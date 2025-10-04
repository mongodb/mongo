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

#pragma once

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Test fixture for shard components, as opposed to config or mongos components. Provides a mock
 * network via ShardingMongoDTestFixture.
 */
class ShardServerTestFixture : public ShardingMongoDTestFixture {
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

class ShardServerTestFixtureWithCatalogCacheMock : public ShardServerTestFixture {
public:
    ShardServerTestFixtureWithCatalogCacheMock() : ShardServerTestFixture() {}
    ShardServerTestFixtureWithCatalogCacheMock(Options options)
        : ShardServerTestFixture(std::move(options)) {}

protected:
    void setUp() override;
    CatalogCacheMock* getCatalogCacheMock();
    std::shared_ptr<ShardServerCatalogCacheLoaderMock> getCatalogCacheLoaderMock();
};

class ShardServerTestFixtureWithCatalogCacheLoaderMock : public ShardServerTestFixture {
protected:
    void setUp() override;
    CatalogCacheMock* getCatalogCacheMock();
    std::shared_ptr<ConfigServerCatalogCacheLoaderMock> getConfigServerCatalogCacheLoaderMock();
    std::shared_ptr<ShardServerCatalogCacheLoaderMock> getCatalogCacheLoaderMock();
};

}  // namespace mongo
