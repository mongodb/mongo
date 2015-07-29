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
#include "mongo/util/net/message_port_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class BSONObj;
class CatalogManagerReplicaSet;
class CollectionType;
class DistLockManagerMock;
class NamespaceString;
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
 * Sets up the mocked out objects for testing the replica-set backed catalog manager.
 */
class CatalogManagerReplSetTestFixture : public mongo::unittest::Test {
public:
    CatalogManagerReplSetTestFixture();
    ~CatalogManagerReplSetTestFixture();

protected:
    static const stdx::chrono::seconds kFutureTimeout;

    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    CatalogManagerReplicaSet* catalogManager() const;

    ShardRegistry* shardRegistry() const;

    RemoteCommandTargeterFactoryMock* targeterFactory() const;

    RemoteCommandTargeterMock* configTargeter() const;

    executor::NetworkInterfaceMock* network() const;

    MessagingPortMock* getMessagingPort() const;

    DistLockManagerMock* distLock() const;

    OperationContext* operationContext() const;

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(executor::NetworkTestEnv::OnCommandFunction func);
    void onFindCommand(executor::NetworkTestEnv::OnFindCommandFunction func);

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
     * Wait for an operation, which creates the sharding change log collection and return the
     * specified response.
     */
    void expectChangeLogCreate(const HostAndPort& configHost, const BSONObj& response);

    /**
     * Wait for a single insert in the change log collection with the specified contents and return
     * a successful response.
     */
    void expectChangeLogInsert(const HostAndPort& configHost,
                               const std::string& clientAddress,
                               Date_t timestamp,
                               const std::string& what,
                               const std::string& ns,
                               const BSONObj& detail);

    /**
     * Expects an update call, which changes the specified collection's namespace contents to match
     * those of the input argument.
     */
    void expectUpdateCollection(const HostAndPort& expectedHost, const CollectionType& coll);

    void setUp() override;

    void tearDown() override;

    void shutdownExecutor();

private:
    std::unique_ptr<ServiceContext> _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<MessagingPortMock> _messagePort;

    RemoteCommandTargeterFactoryMock* _targeterFactory;
    RemoteCommandTargeterMock* _configTargeter;

    executor::NetworkInterfaceMock* _mockNetwork;
    executor::TaskExecutor* _executor;
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;
};

}  // namespace mongo
