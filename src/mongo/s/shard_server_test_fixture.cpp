/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/shard_server_test_fixture.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

const HostAndPort kConfigHostAndPort("dummy", 123);

}  // namespace

ShardServerTestFixture::ShardServerTestFixture() = default;

ShardServerTestFixture::~ShardServerTestFixture() = default;

std::shared_ptr<RemoteCommandTargeterMock> ShardServerTestFixture::configTargeterMock() {
    return RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
}

void ShardServerTestFixture::expectFindOnConfigSendErrorCode(ErrorCodes::Error code) {
    onCommand([&, code](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kConfigHostAndPort);
        ASSERT_EQ(request.dbname, "config");
        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder, Status(code, ""));
        return responseBuilder.obj();
    });
}

void ShardServerTestFixture::expectFindOnConfigSendBSONObjVector(std::vector<BSONObj> obj) {
    onFindCommand([&, obj](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kConfigHostAndPort);
        ASSERT_EQ(request.dbname, "config");
        return obj;
    });
}

void ShardServerTestFixture::setUp() {
    ShardingMongodTestFixture::setUp();

    replicationCoordinator()->alwaysAllowWrites(true);

    // Initialize sharding components as a shard server.
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

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

std::unique_ptr<CatalogCache> ShardServerTestFixture::makeCatalogCache() {
    return stdx::make_unique<CatalogCache>(CatalogCacheLoader::get(getServiceContext()));
}

}  // namespace mongo
