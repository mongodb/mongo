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

#pragma once

#include "mongo/db/server_options.h"
#include "mongo/s/sharding_mongod_test_fixture.h"

namespace mongo {

class RemoteCommandTargeterMock;

/**
 * Test fixture for shard components, as opposed to config or mongos components.
 * Has a mock network and ephemeral storage engine provided by ShardingMongodTestFixture,
 * additionally sets up mock dist lock catalog and manager with a real catalog client.
 */
class ShardServerTestFixture : public ShardingMongodTestFixture {
public:
    ShardServerTestFixture();
    ~ShardServerTestFixture();

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

    void expectFindOnConfigSendErrorCode(ErrorCodes::Error code);

    void expectFindOnConfigSendBSONObjVector(std::vector<BSONObj> obj);

protected:
    /**
     * Sets up a ClusterRole::ShardServer replica set with a real catalog client and mock dist lock
     * catalog and manager.
     */
    void setUp() override;

    void tearDown() override;

    /**
     * Creates a DistLockCatalogMock.
     */
    std::unique_ptr<DistLockCatalog> makeDistLockCatalog() override;

    /**
     * Creates a DistLockManagerMock.
     */
    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override;

    /**
     * Creates a real ShardingCatalogClient.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override;

    std::unique_ptr<CatalogCache> makeCatalogCache() override;
};

}  // namespace mongo
