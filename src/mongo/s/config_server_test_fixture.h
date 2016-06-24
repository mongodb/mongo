/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message_port_mock.h"

namespace mongo {

class BSONObj;
class CatalogCache;
struct ChunkVersion;
class CollectionType;
class ReplSetDistLockManager;
class NamespaceString;
class Shard;
class ShardFactoryMock;
class ShardingCatalogClient;
class ShardingCatalogClientImpl;
class ShardingCatalogManager;
class ShardingCatalogManagerImpl;
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

/**
 * Sets up the mocked out objects for testing the catalog manager and catalog client with the
 * remote interface backed by the NetworkTestEnv and config server as the local storage engine.
 */
class ConfigServerTestFixture : public ServiceContextMongoDTest {
public:
    ConfigServerTestFixture();
    ~ConfigServerTestFixture();

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

    std::shared_ptr<Shard> getConfigShard() const;

    executor::NetworkInterfaceMock* network() const;

    executor::TaskExecutor* executor() const;

    MessagingPortMock* getMessagingPort() const;

    ReplSetDistLockManager* distLock() const;

    OperationContext* operationContext() const;

    /**
     * Insert a document to this config server to the specified namespace.
     */
    Status insertToConfigCollection(OperationContext* txn,
                                    const NamespaceString& ns,
                                    const BSONObj& doc);

    /**
     * Reads a single document from a collection living on the config server.
     */
    StatusWith<BSONObj> findOneOnConfigCollection(OperationContext* txn,
                                                  const NamespaceString& ns,
                                                  const BSONObj& filter);

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
     * Setup the config.shards collection to contain the given shards.
     */
    Status setupShards(const std::vector<ShardType>& shards);

    /**
     * Retrieves the shard document from the config server.
     * Returns {ErrorCodes::ShardNotFound} if the given shard does not exists.
     */
    StatusWith<ShardType> getShardDoc(OperationContext* txn, const std::string& shardId);

    /**
     * Returns the indexes definitions defined on a given collection.
     */
    StatusWith<std::vector<BSONObj>> getIndexes(OperationContext* txn, const NamespaceString& ns);

    void setUp() override;

    void tearDown() override;

    void shutdownExecutor();

    /**
     * Checks that the given command has the expected settings for read after opTime.
     */
    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const;

private:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<MessagingPortMock> _messagePort;

    RemoteCommandTargeterFactoryMock* _targeterFactory;

    executor::NetworkInterfaceMock* _mockNetwork;
    executor::TaskExecutor* _executor;
    executor::TaskExecutor* _executorForAddShard;
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;
    std::unique_ptr<executor::NetworkTestEnv> _addShardNetworkTestEnv;
    ReplSetDistLockManager* _distLockManager = nullptr;
    ShardingCatalogClientImpl* _catalogClient = nullptr;
    ShardingCatalogManagerImpl* _catalogManager = nullptr;
};

}  // namespace mongo
