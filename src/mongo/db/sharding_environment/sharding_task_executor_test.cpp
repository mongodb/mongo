// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/sharding_task_executor.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <list>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
            LogicalSessionFromClient::parse(cmdObj["lsid"].Obj(), IDLParserContext{"lsid"});

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

    const RemoteCommandRequest request(
        HostAndPort("localhost", 27017),
        DatabaseName::createDatabaseName_forTest(boost::none, "mydb"),
        BSON("whatsUp" << "doc"),
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
