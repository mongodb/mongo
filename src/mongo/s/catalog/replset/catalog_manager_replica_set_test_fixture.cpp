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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"

#include <algorithm>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using unittest::assertGet;

using std::string;
using std::vector;
using unittest::assertGet;

CatalogManagerReplSetTestFixture::CatalogManagerReplSetTestFixture() = default;

CatalogManagerReplSetTestFixture::~CatalogManagerReplSetTestFixture() = default;

const stdx::chrono::seconds CatalogManagerReplSetTestFixture::kFutureTimeout{5};

void CatalogManagerReplSetTestFixture::setUp() {
    _service = stdx::make_unique<ServiceContextNoop>();
    _messagePort = stdx::make_unique<MessagingPortMock>();
    _client = _service->makeClient("CatalogManagerReplSetTestFixture", _messagePort.get());
    _opCtx = _client->makeOperationContext();

    auto targeterFactory(stdx::make_unique<RemoteCommandTargeterFactoryMock>());
    _targeterFactory = targeterFactory.get();

    auto network(stdx::make_unique<executor::NetworkInterfaceMock>());
    _mockNetwork = network.get();

    std::unique_ptr<repl::ReplicationExecutor> executor(
        stdx::make_unique<repl::ReplicationExecutor>(network.release(), nullptr, 0));

    _networkTestEnv = stdx::make_unique<NetworkTestEnv>(executor.get(), _mockNetwork);

    std::unique_ptr<CatalogManagerReplicaSet> cm(stdx::make_unique<CatalogManagerReplicaSet>());

    ASSERT_OK(cm->init(
        ConnectionString::forReplicaSet("CatalogManagerReplSetTest",
                                        {HostAndPort{"TestHost1"}, HostAndPort{"TestHost2"}}),
        stdx::make_unique<DistLockManagerMock>()));

    auto configTargeter(stdx::make_unique<RemoteCommandTargeterMock>());
    _configTargeter = configTargeter.get();
    _targeterFactory->addTargeterToReturn(cm->connectionString(), std::move(configTargeter));

    auto shardRegistry(stdx::make_unique<ShardRegistry>(
        std::move(targeterFactory), std::move(executor), _mockNetwork));
    shardRegistry->init(cm.get());
    shardRegistry->startup();

    // For now initialize the global grid object. All sharding objects will be accessible
    // from there until we get rid of it.
    grid.init(std::move(cm), std::move(shardRegistry));
}

void CatalogManagerReplSetTestFixture::tearDown() {
    // This call will shut down the shard registry, which will terminate the underlying executor
    // and its threads.
    grid.clearForUnitTests();

    _opCtx.reset();
    _client.reset();
    _service.reset();
}

CatalogManagerReplicaSet* CatalogManagerReplSetTestFixture::catalogManager() const {
    auto cm = dynamic_cast<CatalogManagerReplicaSet*>(grid.catalogManager());
    invariant(cm);

    return cm;
}

ShardRegistry* CatalogManagerReplSetTestFixture::shardRegistry() const {
    invariant(grid.shardRegistry());

    return grid.shardRegistry();
}

RemoteCommandTargeterFactoryMock* CatalogManagerReplSetTestFixture::targeterFactory() const {
    invariant(_targeterFactory);

    return _targeterFactory;
}

RemoteCommandTargeterMock* CatalogManagerReplSetTestFixture::configTargeter() const {
    invariant(_configTargeter);

    return _configTargeter;
}

executor::NetworkInterfaceMock* CatalogManagerReplSetTestFixture::network() const {
    invariant(_mockNetwork);

    return _mockNetwork;
}

MessagingPortMock* CatalogManagerReplSetTestFixture::getMessagingPort() const {
    return _messagePort.get();
}

DistLockManagerMock* CatalogManagerReplSetTestFixture::distLock() const {
    auto distLock = dynamic_cast<DistLockManagerMock*>(catalogManager()->getDistLockManager());
    invariant(distLock);

    return distLock;
}

OperationContext* CatalogManagerReplSetTestFixture::operationContext() const {
    invariant(_opCtx);

    return _opCtx.get();
}

void CatalogManagerReplSetTestFixture::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void CatalogManagerReplSetTestFixture::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void CatalogManagerReplSetTestFixture::setupShards(const std::vector<ShardType>& shards) {
    auto future = launchAsync([this] { shardRegistry()->reload(); });

    expectGetShards(shards);

    future.timed_get(kFutureTimeout);
}

void CatalogManagerReplSetTestFixture::expectGetShards(const std::vector<ShardType>& shards) {
    onFindCommand([&shards](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), ShardType::ConfigNS);

        auto queryResult = LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(queryResult.getStatus());

        const auto& query = queryResult.getValue();
        ASSERT_EQ(query->ns(), ShardType::ConfigNS);

        ASSERT_EQ(query->getFilter(), BSONObj());
        ASSERT_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().is_initialized());

        vector<BSONObj> shardsToReturn;

        std::transform(shards.begin(),
                       shards.end(),
                       std::back_inserter(shardsToReturn),
                       [](const ShardType& shard) { return shard.toBSON(); });

        return shardsToReturn;
    });
}

void CatalogManagerReplSetTestFixture::expectInserts(const NamespaceString nss,
                                                     const std::vector<BSONObj>& expected) {
    onCommand([&nss, &expected](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(nss.db(), request.dbname);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));

        ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().toString());

        auto inserted = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(expected.size(), inserted.size());

        auto itInserted = inserted.begin();
        auto itExpected = expected.begin();

        for (; itInserted != inserted.end(); itInserted++, itExpected++) {
            ASSERT_EQ(*itExpected, *itInserted);
        }

        BatchedCommandResponse response;
        response.setOk(true);

        return response.toBSON();
    });
}

void CatalogManagerReplSetTestFixture::expectChangeLogCreate(const HostAndPort& configHost,
                                                             const BSONObj& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);
        BSONObj expectedCreateCmd = BSON("create" << ChangeLogType::ConfigNS << "capped" << true
                                                  << "size" << 1024 * 1024 * 10);
        ASSERT_EQUALS(expectedCreateCmd, request.cmdObj);

        return response;
    });
}

void CatalogManagerReplSetTestFixture::expectChangeLogInsert(const HostAndPort& configHost,
                                                             const std::string& clientAddress,
                                                             Date_t timestamp,
                                                             const std::string& what,
                                                             const std::string& ns,
                                                             const BSONObj& detail) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(ChangeLogType::ConfigNS, actualBatchedInsert.getNS().ns());

        auto inserts = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(1U, inserts.size());

        const ChangeLogType& actualChangeLog = assertGet(ChangeLogType::fromBSON(inserts.front()));

        ASSERT_EQUALS(clientAddress, actualChangeLog.getClientAddr());
        ASSERT_EQUALS(detail, actualChangeLog.getDetails());
        ASSERT_EQUALS(ns, actualChangeLog.getNS());
        ASSERT_EQUALS(shardRegistry()->getNetwork()->getHostName(), actualChangeLog.getServer());
        ASSERT_EQUALS(timestamp, actualChangeLog.getTime());
        ASSERT_EQUALS(what, actualChangeLog.getWhat());

        // Handle changeId specially because there's no way to know what OID was generated
        std::string changeId = actualChangeLog.getChangeId();
        size_t firstDash = changeId.find("-");
        size_t lastDash = changeId.rfind("-");

        const std::string serverPiece = changeId.substr(0, firstDash);
        const std::string timePiece = changeId.substr(firstDash + 1, lastDash - firstDash - 1);
        const std::string oidPiece = changeId.substr(lastDash + 1);

        ASSERT_EQUALS(shardRegistry()->getNetwork()->getHostName(), serverPiece);
        ASSERT_EQUALS(timestamp.toString(), timePiece);

        OID generatedOID;
        // Just make sure this doesn't throws and assume the OID is valid
        generatedOID.init(oidPiece);

        BatchedCommandResponse response;
        response.setOk(true);

        return response.toBSON();
    });
}

void CatalogManagerReplSetTestFixture::expectCount(const HostAndPort& configHost,
                                                   const NamespaceString& expectedNs,
                                                   const BSONObj& expectedQuery,
                                                   const StatusWith<long long>& response) {
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("count", cmdName);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQUALS(expectedNs.toString(), nss.toString());

        if (expectedQuery.isEmpty()) {
            ASSERT_TRUE(request.cmdObj["query"].eoo());
        } else {
            ASSERT_EQUALS(expectedQuery, request.cmdObj["query"].Obj());
        }

        if (response.isOK()) {
            return BSON("ok" << 1 << "n" << response.getValue());
        }

        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder, response.getStatus());
        return responseBuilder.obj();
    });
}

}  // namespace mongo
