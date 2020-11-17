/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/executor/mock_network_fixture.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_interface_mock_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace executor;
using namespace test::mock;

class MockNetworkTest : public NetworkInterfaceMockTest {
public:
    MockNetworkTest() : NetworkInterfaceMockTest(), _mock(&NetworkInterfaceMockTest::net()){};

    MockNetwork& mock() {
        return _mock;
    }

    void setUp() override {
        NetworkInterfaceMockTest::setUp();
        NetworkInterfaceMockTest::startNetwork();
    }

    void tearDown() override {
        NetworkInterfaceMockTest::tearDown();
        // Will check for unsatisfied expectations.
        mock().verifyAndClearExpectations();
    }

    void evaluateResponse(const RemoteCommandOnAnyResponse& resp, BSONObj expectedResponse) {
        LOGV2(5015503, "Test got command response", "resp"_attr = resp);
        ASSERT(resp.isOK());
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expectedResponse == resp.data));
    }

    std::string kExampleCmdName = "someCommandName";
    RemoteCommandRequestOnAny kExampleRequest{
        {testHost()}, "testDB", BSON(kExampleCmdName << 1), rpc::makeEmptyMetadata(), nullptr};
    BSONObj kExampleResponse = BSON("some"
                                    << "response");

private:
    MockNetwork _mock;
};

TEST_F(MockNetworkTest, MockFixtureBasicTest) {
    mock().expect(kExampleCmdName, kExampleResponse);

    RemoteCommandRequestOnAny request{kExampleRequest};
    bool commandFinished = false;

    TaskExecutor::CallbackHandle cb;
    auto finishFn = [&](const RemoteCommandOnAnyResponse& resp) {
        evaluateResponse(resp, kExampleResponse);
        commandFinished = true;
    };
    ASSERT_OK(net().startCommand(cb, request, finishFn));

    mock().runUntilExpectationsSatisfied();
    ASSERT(commandFinished);
}

}  // namespace
}  // namespace mongo