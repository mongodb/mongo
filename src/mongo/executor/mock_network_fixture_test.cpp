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


#include "mongo/executor/mock_network_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_interface_mock_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using namespace executor;
using namespace test::mock;

class MockNetworkTest : public NetworkInterfaceMockTest {
public:
    MockNetworkTest() : NetworkInterfaceMockTest(), _mock(&NetworkInterfaceMockTest::net()) {};

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

    Status scheduleCommand(RemoteCommandRequest& request) {
        TaskExecutor::CallbackHandle cb;

        try {
            net()
                .startCommand(cb, request, nullptr, _source.token())
                .unsafeToInlineFuture()
                .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
                    auto resp = swRcr.getValue();
                    LOGV2(5015503, "Test got command response", "resp"_attr = resp);
                    _responses.push_back(resp);
                });
        } catch (const DBException& e) {
            return e.toStatus();
        }

        return Status::OK();
    }

    // Assumes standard responses (kExampleResponse).
    void evaluateResponses(const int numExpected) {
        ASSERT_EQUALS(numExpected, _responses.size());
        for (const auto& resp : _responses) {
            ASSERT(resp.isOK());
            ASSERT(SimpleBSONObjComparator::kInstance.evaluate(kExampleResponse == resp.data));
        }
    }

    std::string kExampleCmdName = "someCommandName";
    std::string kExampleCmdNameTwo = kExampleCmdName + "_two";
    std::string kExampleCmdNameThree = kExampleCmdName + "_three";
    std::string kExampleCmdNameFour = kExampleCmdName + "_four";

    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB");
    RemoteCommandRequest kExampleRequest{
        {testHost()}, dbName, BSON(kExampleCmdName << 1), rpc::makeEmptyMetadata(), nullptr};
    RemoteCommandRequest kExampleRequestTwo{
        {testHost()}, dbName, BSON(kExampleCmdNameTwo << 1), rpc::makeEmptyMetadata(), nullptr};
    RemoteCommandRequest kExampleRequestThree{
        {testHost()}, dbName, BSON(kExampleCmdNameThree << 1), rpc::makeEmptyMetadata(), nullptr};
    RemoteCommandRequest kExampleRequestFour{
        {testHost()}, dbName, BSON(kExampleCmdNameFour << 1), rpc::makeEmptyMetadata(), nullptr};

    BSONObj kExampleResponse = BSON("some" << "response");

    RemoteCommandRequest makeRequest(std::string cmdName) {
        return {{testHost()}, dbName, BSON(cmdName << 1), rpc::makeEmptyMetadata(), nullptr};
    }

    RemoteCommandRequest makeRequest(BSONObj obj) {
        return {{testHost()}, dbName, obj, rpc::makeEmptyMetadata(), nullptr};
    }

    void cancelCommand() {
        _source.cancel();
    }

private:
    MockNetwork _mock;
    std::vector<RemoteCommandResponse> _responses;
    CancellationSource _source;
};

TEST_F(MockNetworkTest, MockFixtureBasicTest) {
    mock().expect(kExampleCmdName, kExampleResponse);

    RemoteCommandRequest request{kExampleRequest};
    ASSERT_OK(scheduleCommand(request));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(1 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureBasicTestWithMatcherFn) {
    mock().expect(
        [&](auto& request) { return request.firstElementFieldNameStringData() == kExampleCmdName; },
        kExampleResponse);

    RemoteCommandRequest request{kExampleRequest};
    ASSERT_OK(scheduleCommand(request));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(1 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureBasicTestSameCommandMultipleTimes) {
    mock().expect(kExampleCmdName, kExampleResponse).times(3);

    RemoteCommandRequest request{kExampleRequest};

    // Run command three times.
    ASSERT_OK(scheduleCommand(request));
    ASSERT_OK(scheduleCommand(request));
    ASSERT_OK(scheduleCommand(request));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(3 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSeveralExpectationsUnordered) {
    // We will interleave requests for these expectations.
    // Order of requests will be: Two, One, Two, Two, One.
    mock().expect(kExampleCmdName, kExampleResponse).times(2);
    mock().expect(kExampleCmdNameTwo, kExampleResponse).times(3);

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestOne));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(5 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSimilarExpectationsSpecialization) {
    // These expectations both share the same command name, but the second one has more info.
    // Generally, latter expectations should override previous ones if they are similar enough.
    // We are testing here that they both are still satisfied as we end up evaluating the more
    // specific one first.
    mock().expect(kExampleCmdName, kExampleResponse).times(1);
    mock().expect(BSON(kExampleCmdName << 1 << "extradata" << 1), kExampleResponse).times(1);

    RemoteCommandRequest cmdNameRequest{kExampleRequest};
    RemoteCommandRequest bsonRequest{makeRequest(BSON(kExampleCmdName << 1 << "extradata" << 1))};

    // Run commands starting with the one for the older (potentially overridden) expectations.
    ASSERT_OK(scheduleCommand(cmdNameRequest));
    ASSERT_OK(scheduleCommand(bsonRequest));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(2 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSimilarExpectationsOverride) {
    // Add a default expectation so we can still cover the overridden command.
    // This is necessary as unanswered requests will cause the test to hang.
    mock().defaultExpect(kExampleCmdName, kExampleResponse);

    // These expectations both share the same command name, but the first one has more info.
    // This test demonstrates the case where we override a specific expectation with a more
    // general one. Namely, it shows the pitfalls of doing so - the more general request
    // will match greedily here, displacing the more specific one.
    auto specificExp =
        mock().expect(BSON(kExampleCmdName << 1 << "extradata" << 1), kExampleResponse).times(1);
    mock().expect(kExampleCmdName, kExampleResponse).times(1);

    RemoteCommandRequest bsonRequest{makeRequest(BSON(kExampleCmdName << 1 << "extradata" << 1))};
    RemoteCommandRequest cmdNameRequest{kExampleRequest};

    // Run commands starting with the one for the older (potentially overridden) expectations.
    // The first request is meant for the older expectation, however it fulfills the matching
    // requirements for the more recent one. This is a problem as the remaining user expectation
    // will be unable to match the second request (as it is missing the extra field).
    ASSERT_OK(scheduleCommand(bsonRequest));
    // We will not be able to match this request to a user expectation at this point, so it will
    // have to use the default expectation.
    ASSERT_OK(scheduleCommand(cmdNameRequest));

    const auto deadline = net().now() + Milliseconds(100);
    mock().runUntil(deadline);

    // The command matcher superseded the BSON matcher so we have an some unmatched expectation.
    ASSERT_THROWS_CODE(mock().verifyExpectations(), DBException, (ErrorCodes::Error)5015501);
    ASSERT(!specificExp.isSatisfied());

    // Fix the tally to meet all expectations.
    ASSERT_OK(scheduleCommand(bsonRequest));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(3 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureRunUntilReadyRequest) {
    mock().expect(kExampleCmdName, kExampleResponse);

    RemoteCommandRequest request{kExampleRequest};
    ASSERT_OK(scheduleCommand(request));

    TaskExecutor::CallbackHandle cbAlarm;
    bool alarmFired = false;
    const auto deadline = net().now() + Milliseconds(100);
    net().setAlarm(deadline).unsafeToInlineFuture().getAsync([&](Status status) {
        ASSERT(status.isOK());
        alarmFired = true;
    });
    ASSERT_FALSE(alarmFired);

    mock().runUntil(deadline);
    ASSERT_TRUE(alarmFired);

    // We will have run our expected request as it was ready.
    mock().verifyExpectations();
    evaluateResponses(1 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureRunUntilNotAllExpectationsSatisfied) {
    mock().expect(kExampleCmdName, kExampleResponse);
    mock().expect(kExampleCmdNameTwo, kExampleResponse);

    RemoteCommandRequest request{kExampleRequest};
    ASSERT_OK(scheduleCommand(request));

    TaskExecutor::CallbackHandle cbAlarm;
    bool alarmFired = false;
    const auto deadline = net().now() + Milliseconds(100);
    net().setAlarm(deadline).unsafeToInlineFuture().getAsync([&](Status status) {
        ASSERT(status.isOK());
        alarmFired = true;
    });
    ASSERT_FALSE(alarmFired);

    mock().runUntil(deadline);
    ASSERT_TRUE(alarmFired);

    // We had only one ready request to run. The one corresponding to the other
    // expectation does not exist yet.
    ASSERT_THROWS_CODE(mock().verifyExpectations(), DBException, (ErrorCodes::Error)5015501);

    // Satisfy the other expectation to terminate the test.
    RemoteCommandRequest requestTwo{kExampleRequestTwo};
    ASSERT_OK(scheduleCommand(requestTwo));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(2 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureNotEnoughTimesMatched) {
    mock().expect(kExampleCmdName, kExampleResponse).times(2);

    RemoteCommandRequest request{kExampleRequest};
    ASSERT_OK(scheduleCommand(request));

    const auto deadline = net().now() + Milliseconds(100);
    mock().runUntil(deadline);

    // We expect to serve the same request twice but we've only received it once so far.
    ASSERT_THROWS_CODE(mock().verifyExpectations(), DBException, (ErrorCodes::Error)5015501);

    ASSERT_OK(scheduleCommand(request));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(2 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestCorrectOrder) {
    // The requests will come in the expected order.
    {
        MockNetwork::InSequence seq(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
    }

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestTwo));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(2 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestOppositeOrder) {
    // We expect One -> Two, but the requests will come in the opposite order.
    // We will refuse to match Two to its user expectation if One is not satisfied,
    // so we make a default expectation to fall back to so the test can move forward.
    mock().defaultExpect(kExampleCmdNameTwo, kExampleResponse);

    {
        MockNetwork::InSequence seq(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
    }

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestOne));

    mock().runUntil(net().now() + Milliseconds(100));

    // The second expectation will not have been satisfied, as the first one has not yet been
    // met at that time it came in.
    ASSERT_THROWS_CODE(mock().verifyExpectations(), DBException, (ErrorCodes::Error)5015501);

    // Now we can send a request for expectation Two again.
    ASSERT_OK(scheduleCommand(requestTwo));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(3 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestCorrectPartialOrder) {
    // We only require One -> Two, so One -> Three -> Two is valid.
    {
        MockNetwork::InSequence seq(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
    }

    mock().expect(kExampleCmdNameThree, kExampleResponse).times(1);

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};
    RemoteCommandRequest requestThree{kExampleRequestThree};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestThree));
    ASSERT_OK(scheduleCommand(requestTwo));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(3 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestTwoSequencesNoOverlap) {
    // We have One -> Two and Three -> Four. We run One -> Two -> Three -> Four;
    {
        MockNetwork::InSequence seqOne(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
    }

    {
        MockNetwork::InSequence seqTwo(mock());

        mock().expect(kExampleCmdNameThree, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameFour, kExampleResponse).times(1);
    }

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};
    RemoteCommandRequest requestThree{kExampleRequestThree};
    RemoteCommandRequest requestFour{kExampleRequestFour};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestThree));
    ASSERT_OK(scheduleCommand(requestFour));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(4 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestTwoSequencesPartialOrder) {
    // We have One -> Two and Three -> Four. We run One -> Three -> Two -> Four;
    {
        MockNetwork::InSequence seqOne(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
    }

    {
        MockNetwork::InSequence seqTwo(mock());

        mock().expect(kExampleCmdNameThree, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameFour, kExampleResponse).times(1);
    }

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};
    RemoteCommandRequest requestThree{kExampleRequestThree};
    RemoteCommandRequest requestFour{kExampleRequestFour};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestThree));
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestFour));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(4 /* numExpected */);
}

TEST_F(MockNetworkTest, MockFixtureSequenceTestLongerChain) {
    // One -> Two -> Three -> Four required.
    {
        MockNetwork::InSequence seqOne(mock());

        mock().expect(kExampleCmdName, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameTwo, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameThree, kExampleResponse).times(1);
        mock().expect(kExampleCmdNameFour, kExampleResponse).times(1);
    }

    RemoteCommandRequest requestOne{kExampleRequest};
    RemoteCommandRequest requestTwo{kExampleRequestTwo};
    RemoteCommandRequest requestThree{kExampleRequestThree};
    RemoteCommandRequest requestFour{kExampleRequestFour};

    // Run commands in this specific order.
    ASSERT_OK(scheduleCommand(requestOne));
    ASSERT_OK(scheduleCommand(requestTwo));
    ASSERT_OK(scheduleCommand(requestThree));
    ASSERT_OK(scheduleCommand(requestFour));

    mock().runUntilExpectationsSatisfied();
    evaluateResponses(4 /* numExpected */);
}

}  // namespace
}  // namespace mongo
