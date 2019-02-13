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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/sharding_test_fixture_common.h"

namespace mongo {

class BSONObj;
class ShardingCatalogClient;
struct ChunkVersion;
class CollectionType;
class DistLockManagerMock;
class RemoteCommandTargeterFactoryMock;
class RemoteCommandTargeterMock;
class ShardRegistry;
class ShardType;

namespace transport {
class TransportLayerMock;
}  // namespace transport

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager and catalog
 * client.
 */
class ShardingTestFixture : public ServiceContextTest, public ShardingTestFixtureCommon {
public:
    ShardingTestFixture();
    ~ShardingTestFixture();

    // Syntactic sugar for getting sharding components off the Grid, if they have been initialized.

    ShardingCatalogClient* catalogClient() const;
    ShardRegistry* shardRegistry() const;
    RemoteCommandTargeterFactoryMock* targeterFactory() const;
    executor::TaskExecutor* executor() const;
    DistLockManagerMock* distLock() const;
    RemoteCommandTargeterMock* configTargeter() const;

    OperationContext* operationContext() const;

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
     *
     */
    void expectFindSendBSONObjVector(const HostAndPort& configHost, std::vector<BSONObj> obj);

    /**
     * Waits for an operation which creates a capped config collection with the specified name and
     * capped size.
     */
    void expectConfigCollectionCreate(const HostAndPort& configHost,
                                      StringData collName,
                                      int cappedSize,
                                      const BSONObj& response);

    /**
     * Wait for a single insert in one of the change or action log collections with the specified
     * contents and return a successful response.
     */
    void expectConfigCollectionInsert(const HostAndPort& configHost,
                                      StringData collName,
                                      Date_t timestamp,
                                      const std::string& what,
                                      const std::string& ns,
                                      const BSONObj& detail);

    /**
     * Wait for the config.changelog collection to be created on the specified host.
     */
    void expectChangeLogCreate(const HostAndPort& configHost, const BSONObj& response);

    /**
     * Expect a log message with the specified contents to be written to the config.changelog
     * collection.
     */
    void expectChangeLogInsert(const HostAndPort& configHost,
                               Date_t timestamp,
                               const std::string& what,
                               const std::string& ns,
                               const BSONObj& detail);

    /**
     * Expects an update call, which changes the specified collection's namespace contents to match
     * those of the input argument.
     */
    void expectUpdateCollection(const HostAndPort& expectedHost,
                                const CollectionType& coll,
                                bool expectUpsert = true);

    /**
     * Expects a setShardVersion command to be executed on the specified shard.
     */
    void expectSetShardVersion(const HostAndPort& expectedHost,
                               const ShardType& expectedShard,
                               const NamespaceString& expectedNs,
                               const ChunkVersion& expectedChunkVersion);

    void shutdownExecutor();

    void setRemote(const HostAndPort& remote);

    /**
     * Checks that the given command has the expected settings for read after opTime.
     */
    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const;

private:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    transport::SessionHandle _transportSession;

    RemoteCommandTargeterFactoryMock* _targeterFactory;
    RemoteCommandTargeterMock* _configTargeter;

    // For the Grid's fixed executor.
    executor::TaskExecutor* _executor;

    // For the Grid's arbitrary executor in its executorPool.
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnvForPool;

    DistLockManagerMock* _distLockManager = nullptr;
};

}  // namespace mongo
