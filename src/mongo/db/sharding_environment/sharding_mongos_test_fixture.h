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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <tuple>
#include <vector>

namespace mongo {

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager and catalog
 * client.
 */
class ShardingTestFixture : public ShardingTestFixtureCommon {
protected:
    ShardingTestFixture();
    explicit ShardingTestFixture(bool withMockCatalogCache)
        : ShardingTestFixture{withMockCatalogCache, nullptr} {}
    ShardingTestFixture(bool withMockCatalogCache,
                        std::unique_ptr<ScopedGlobalServiceContextForTest> scopedGlobalContext);

    ~ShardingTestFixture() override;

    /**
     * Returns the mock targeter for the config server. Useful to use like so,
     *
     *     configTargeterMock()->setFindHostReturnValue(HostAndPort);
     *     configTargeterMock()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"})
     *
     * Remote calls always need to resolve a host with RemoteCommandTargeterMock::findHost, so it
     * must be set.
     */
    std::shared_ptr<RemoteCommandTargeterMock> configTargeter();

    // Syntactic sugar for getting sharding components off the Grid, if they have been initialized.

    ShardingCatalogClient* catalogClient() const;
    ShardRegistry* shardRegistry() const;
    std::shared_ptr<executor::TaskExecutor> executor() const;

    /**
     * Same as the onCommand* variants, but expects the request to be placed on the arbitrary
     * executor of the Grid's executorPool.
     */
    void onCommandForPoolExecutor(executor::NetworkTestEnv::OnCommandFunction func);

    /**
     * Setup the shard registry to contain the given shards until the next reload.
     */
    void setupShards(const std::vector<ShardType>& shards);

    /**
     * Adds ShardRemote shards to the shard registry.
     */
    void addRemoteShards(const std::vector<std::tuple<ShardId, HostAndPort>>& shards);

    /**
     * Wait for the shards listing command to be run and returns the specified set of shards.
     */
    void expectGetShards(const std::vector<ShardType>& shards);

    /**
     * Wait for a single insert request and ensures that the items being inserted exactly match the
     * expected items. Responds with a success status.
     */
    void expectInserts(const NamespaceString& nss, const std::vector<BSONObj>& expected);

    /**
     * Waits for a count command and returns a response reporting the given number of documents
     * as the result of the count, or an error.
     */
    void expectCount(const HostAndPort& configHost,
                     const NamespaceString& expectedNs,
                     const BSONObj& expectedQuery,
                     const StatusWith<long long>& response);

    /**
     * Expects a find command on configHost's 'config' database and returns an array of objects
     * 'obj' as a response.
     */
    void expectFindSendBSONObjVector(const HostAndPort& configHost, std::vector<BSONObj> obj);

    /**
     * Expects an update call, which changes the specified collection's namespace contents to match
     * those of the input argument.
     */
    void expectUpdateCollection(const HostAndPort& expectedHost,
                                const CollectionType& coll,
                                bool expectUpsert = true);

    void shutdownExecutor();

    /**
     * Checks that the given command has the expected settings for read after opTime.
     */
    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const;

    /**
     * Mocks an error cursor response from a remote with the given 'status'.
     */
    BSONObj createErrorCursorResponse(Status status) {
        invariant(!status.isOK());
        BSONObjBuilder result;
        status.serializeErrorToBSON(&result);
        result.appendBool("ok", false);
        return result.obj();
    }

private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override;

    // For the Grid's fixed executor.
    std::shared_ptr<executor::TaskExecutor> _fixedExecutor;

    // For the Grid's arbitrary executor in its executorPool.
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnvForPool;
};

class ShardingTestFixtureWithMockCatalogCache : public ShardingTestFixture {
public:
    ShardingTestFixtureWithMockCatalogCache()
        : ShardingTestFixture(true /*withMockCatalogCache*/) {}

    CatalogCacheMock* getCatalogCacheMock() {
        return checked_cast<CatalogCacheMock*>(Grid::get(operationContext())->catalogCache());
    }
};

}  // namespace mongo
