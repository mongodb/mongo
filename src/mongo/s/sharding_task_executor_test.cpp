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


#include <fmt/format.h>
#include <list>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost("FakeConfigHost", 12345);

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::TaskExecutor;

LogicalSessionId constructFullLsid() {
    auto id = UUID::gen();
    auto uid = SHA256Block{};

    return LogicalSessionId(id, uid);
}

class ShardingTaskExecutorTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        auto netForFixedTaskExecutor = std::make_unique<executor::NetworkInterfaceMock>();
        _network = netForFixedTaskExecutor.get();

        _executor = executor::ShardingTaskExecutor::create(
            makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor)));
        _executor->startup();
    }

    void tearDown() override {
        ShardingTestFixture::tearDown();
        _executor->shutdown();
        executor::NetworkInterfaceMock::InNetworkGuard(_network)->runReadyNetworkOperations();
        _executor->join();
    }

    void assertOpCtxLsidEqualsCmdObjLsid(const BSONObj& cmdObj) {
        auto opCtxLsid = operationContext()->getLogicalSessionId();

        ASSERT(opCtxLsid);

        auto cmdObjLsid =
            LogicalSessionFromClient::parse(IDLParserContext{"lsid"}, cmdObj["lsid"].Obj());

        ASSERT_EQ(opCtxLsid->getId(), cmdObjLsid.getId());
        ASSERT_EQ(opCtxLsid->getUid(), *cmdObjLsid.getUid());
    }

    std::shared_ptr<executor::ShardingTaskExecutor>& getExecutor() {
        return _executor;
    }

    executor::NetworkInterfaceMock* _network{nullptr};

    std::shared_ptr<executor::ShardingTaskExecutor> _executor;
};

TEST_F(ShardingTaskExecutorTest, MissingLsidAddsLsidInCommand) {
    operationContext()->setLogicalSessionId(constructFullLsid());
    ASSERT(operationContext()->getLogicalSessionId());

    NetworkInterfaceMock::InNetworkGuard ing(_network);

    const RemoteCommandRequest request(HostAndPort("localhost", 27017),
                                       DatabaseName::createDatabaseName_forTest(boost::none,
                                                                                "mydb"),
                                       BSON("whatsUp"
                                            << "doc"),
                                       operationContext());

    TaskExecutor::CallbackHandle cbHandle =
        unittest::assertGet(getExecutor()->scheduleRemoteCommand(
            request,
            [=](const executor::TaskExecutor::RemoteCommandCallbackArgs) -> void {},
            nullptr));

    ASSERT(_network->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = _network->getNextReadyRequest();
    auto cmdObj = noi->getRequest().cmdObj;

    assertOpCtxLsidEqualsCmdObjLsid(cmdObj);
}

TEST_F(ShardingTaskExecutorTest, IncompleteLsidAddsLsidInCommand) {
    operationContext()->setLogicalSessionId(constructFullLsid());
    ASSERT(operationContext()->getLogicalSessionId());

    NetworkInterfaceMock::InNetworkGuard ing(_network);

    BSONObjBuilder bob;
    bob.append("whatsUp", "doc");
    {
        BSONObjBuilder subbob(bob.subobjStart("lsid"));
        subbob << "id" << operationContext()->getLogicalSessionId()->getId();
        subbob.done();
    }

    const RemoteCommandRequest request(
        HostAndPort("localhost", 27017),
        DatabaseName::createDatabaseName_forTest(boost::none, "mydb"),
        bob.obj(),
        operationContext());

    TaskExecutor::CallbackHandle cbHandle =
        unittest::assertGet(getExecutor()->scheduleRemoteCommand(
            request,
            [=](const executor::TaskExecutor::RemoteCommandCallbackArgs) -> void {},
            nullptr));

    ASSERT(_network->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = _network->getNextReadyRequest();
    auto cmdObj = noi->getRequest().cmdObj;

    assertOpCtxLsidEqualsCmdObjLsid(cmdObj);
}

}  // namespace
}  // namespace mongo
