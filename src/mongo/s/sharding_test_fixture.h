/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <utility>

#include "mongo/db/service_context.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message_port_mock.h"

namespace mongo {

class BSONObj;
class CatalogCache;
class ShardingCatalogClient;
class ShardingCatalogClientImpl;
class ShardingCatalogManager;
class ShardingCatalogManagerImpl;
struct ChunkVersion;
class CollectionType;
class DistLockManagerMock;
class NamespaceString;
class ShardFactoryMock;
class RemoteCommandTargeterFactoryMock;
class RemoteCommandTargeterMock;
class ShardRegistry;
class ShardType;
template <typename T>
class StatusWith;

namespace executor {
class NetworkInterfaceMock;
class TaskExecutor;
}  // namespace executor

namespace transport {
class TransportLayerMock;
}  // namepsace transport

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager and catalog
 * client.
 */
class ShardingTestFixture : public mongo::unittest::Test {
public:
    ShardingTestFixture();
    ~ShardingTestFixture();

protected:
    static const Seconds kFutureTimeout;

    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    ShardingCatalogClient* catalogClient() const;

    ShardingCatalogManager* catalogManager() const;

    /**
     * Prefer catalogClient() method over this as much as possible.
     */
    ShardingCatalogClientImpl* getCatalogClient() const;

    ShardRegistry* shardRegistry() const;

    RemoteCommandTargeterFactoryMock* targeterFactory() const;

    RemoteCommandTargeterMock* configTargeter() const;

    executor::NetworkInterfaceMock* network() const;

    executor::TaskExecutor* executor() const;

    transport::Session* getTransportSession() const;

    DistLockManagerMock* distLock() const;

    OperationContext* operationContext() const;

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(executor::NetworkTestEnv::OnCommandFunction func);
    // Same as onCommand but run against _addShardNetworkTestEnv
    void onCommandForAddShard(executor::NetworkTestEnv::OnCommandFunction func);
    void onCommandWithMetadata(executor::NetworkTestEnv::OnCommandWithMetadataFunction func);
    void onFindCommand(executor::NetworkTestEnv::OnFindCommandFunction func);
    void onFindWithMetadataCommand(
        executor::NetworkTestEnv::OnFindCommandWithMetadataFunction func);

    /**
     * Setup the shard registry to contain the given shards until the next reload.
     */
    void setupShards(const std::vector<ShardType>& shards);

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
     * Expects an update call, which changes the specified collection's namespace contents to
     * match
     * those of the input argument.
     */
    void expectUpdateCollection(const HostAndPort& expectedHost, const CollectionType& coll);

    /**
     * Expects a setShardVersion command to be executed on the specified shard.
     */
    void expectSetShardVersion(const HostAndPort& expectedHost,
                               const ShardType& expectedShard,
                               const NamespaceString& expectedNs,
                               const ChunkVersion& expectedChunkVersion);

    void setUp() override;

    void tearDown() override;

    void shutdownExecutor();

    void setRemote(const HostAndPort& remote);

    /**
     * Checks that the given command has the expected settings for read after opTime.
     */
    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const;

private:
    std::unique_ptr<ServiceContext> _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    transport::TransportLayerMock* _transportLayer;
    std::unique_ptr<transport::Session> _transportSession;

    RemoteCommandTargeterFactoryMock* _targeterFactory;
    RemoteCommandTargeterMock* _configTargeter;

    executor::NetworkInterfaceMock* _mockNetwork;
    executor::TaskExecutor* _executor;
    executor::TaskExecutor* _executorForAddShard;
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;
    std::unique_ptr<executor::NetworkTestEnv> _addShardNetworkTestEnv;
    DistLockManagerMock* _distLockManager = nullptr;
    ShardingCatalogClientImpl* _catalogClient = nullptr;
    ShardingCatalogManagerImpl* _catalogManager = nullptr;
};

}  // namespace mongo
