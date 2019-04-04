/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/s/shard_server_test_fixture.h"

#include "merizo/client/remote_command_targeter_mock.h"
#include "merizo/db/commands.h"
#include "merizo/db/repl/replication_coordinator_mock.h"
#include "merizo/db/s/shard_server_catalog_cache_loader.h"
#include "merizo/db/s/sharding_state.h"
#include "merizo/s/catalog/dist_lock_catalog_mock.h"
#include "merizo/s/catalog/dist_lock_manager_mock.h"
#include "merizo/s/catalog/sharding_catalog_client_impl.h"
#include "merizo/s/catalog_cache.h"
#include "merizo/s/config_server_catalog_cache_loader.h"
#include "merizo/stdx/memory.h"

namespace merizo {
namespace {

const HostAndPort kConfigHostAndPort("dummy", 123);

}  // namespace

ShardServerTestFixture::ShardServerTestFixture() = default;

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

    CatalogCacheLoader::set(getServiceContext(),
                            stdx::make_unique<ShardServerCatalogCacheLoader>(
                                stdx::make_unique<ConfigServerCatalogCacheLoader>()));

    uassertStatusOK(
        initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort)));

    // Set the findHost() return value on the mock targeter so that later calls to the
    // config targeter's findHost() return the appropriate value.
    configTargeterMock()->setFindHostReturnValue(kConfigHostAndPort);
}

void ShardServerTestFixture::tearDown() {
    CatalogCacheLoader::clearForTests(getServiceContext());

    ShardingMongodTestFixture::tearDown();
}

std::unique_ptr<DistLockCatalog> ShardServerTestFixture::makeDistLockCatalog() {
    return stdx::make_unique<DistLockCatalogMock>();
}

std::unique_ptr<DistLockManager> ShardServerTestFixture::makeDistLockManager(
    std::unique_ptr<DistLockCatalog> distLockCatalog) {
    invariant(distLockCatalog);
    return stdx::make_unique<DistLockManagerMock>(std::move(distLockCatalog));
}

std::unique_ptr<ShardingCatalogClient> ShardServerTestFixture::makeShardingCatalogClient(
    std::unique_ptr<DistLockManager> distLockManager) {
    invariant(distLockManager);
    return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
}

}  // namespace merizo
