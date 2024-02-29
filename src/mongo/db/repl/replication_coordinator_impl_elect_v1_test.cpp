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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <list>
#include <memory>
#include <ratio>
#include <string>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/vote_requester.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/mock_network_fixture.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {
namespace {

using namespace mongo::test::mock;

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using ApplierState = ReplicationCoordinator::ApplierState;

class ReplCoordMockTest : public ReplCoordTest {
public:
    void setUp() override {
        ReplCoordTest::setUp();
        BSONObj configObj = BSON("_id"
                                 << "mySet"
                                 << "version" << 1 << "members"
                                 << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                          << "node1:12345")
                                               << BSON("_id" << 2 << "host"
                                                             << "node2:12345")
                                               << BSON("_id" << 3 << "host"
                                                             << "node3:12345"))
                                 << "protocolVersion" << 1);
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplSetConfig config = assertMakeRSConfig(configObj);

        OpTime time1(Timestamp(100, 1), 0);
        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1,
                                                            Date_t() + Seconds(time1.getSecs()));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        simulateEnoughHeartbeatsForAllNodesUp();

        _mock = std::make_unique<MockNetwork>(getNet());

        // Heartbeat default behavior.
        OpTime lastApplied(Timestamp(100, 1), 0);
        ReplSetHeartbeatResponse hbResp;
        auto rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
        hbResp.setSetName(rsConfig.getReplSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        hbResp.setConfigTerm(rsConfig.getConfigTerm());
        hbResp.setAppliedOpTimeAndWallTime(
            {lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
        hbResp.setWrittenOpTimeAndWallTime(
            {lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
        hbResp.setDurableOpTimeAndWallTime(
            {lastApplied, Date_t() + Seconds(lastApplied.getSecs())});

        _mock->defaultExpect("replSetHeartbeat", hbResp.toBSON());
    };
    void tearDown() override {
        ReplCoordTest::tearDown();
        _mock.reset();
    };

protected:
    std::unique_ptr<MockNetwork> _mock;
};

TEST(LastVote, LastVoteAcceptsUnknownField) {
    auto lastVoteBSON =
        BSON("candidateIndex" << 1 << "term" << 2 << "_id" << 3 << "unknownField" << 1);
    auto lastVoteSW = LastVote::readFromLastVote(lastVoteBSON);
    ASSERT_OK(lastVoteSW.getStatus());
    ASSERT_BSONOBJ_EQ(lastVoteSW.getValue().toBSON(), BSON("term" << 2 << "candidateIndex" << 1));
}

TEST_F(ReplCoordTest, RandomizedElectionOffsetWithinProperBounds) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    Milliseconds electionTimeout = config.getElectionTimeoutPeriod();
    long long randomOffsetUpperBound = durationCount<Milliseconds>(electionTimeout) *
        getExternalState()->getElectionTimeoutOffsetLimitFraction();
    Milliseconds randomOffset;

    // Verify for numerous rounds of random number generation.
    int rounds = 1000;
    for (int i = 0; i < rounds; i++) {
        randomOffset = getReplCoord()->getRandomizedElectionOffset_forTest();
        ASSERT_GREATER_THAN_OR_EQUALS(randomOffset, Milliseconds(0));
        ASSERT_LESS_THAN_OR_EQUALS(randomOffset, Milliseconds(randomOffsetUpperBound));
    }
}

TEST_F(ReplCoordTest, RandomizedElectionOffsetAvoidsDivideByZero) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1 << "settings"
                             << BSON("electionTimeoutMillis" << 1));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));

    // Make sure that an election timeout of 1ms doesn't make the random number
    // generator attempt to divide by zero.
    Milliseconds randomOffset = getReplCoord()->getRandomizedElectionOffset_forTest();
    ASSERT_EQ(Milliseconds(0), randomOffset);
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenNodeIsTheOnlyElectableNode) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345"
                                         << "votes" << 0 << "hidden" << true << "priority" << 0))
             << "protocolVersion" << 1),
        HostAndPort("node1", 12345));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    ASSERT(getReplCoord()->getMemberState().secondary())
        << getReplCoord()->getMemberState().toString();

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(10, 1), 0),
                                                        Date_t() + Seconds(10));

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21453,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->now() < electionTimeoutWhen) {
        net->runUntil(electionTimeoutWhen);
        if (!net->hasReadyRequests()) {
            continue;
        }
        auto noi = net->getNextReadyRequest();
        const auto& request = noi->getRequest();
        LOGV2_ERROR(21473,
                    "Black holing irrelevant request to {request_target}: {request_cmdObj}",
                    "request_target"_attr = request.target,
                    "request_cmdObj"_attr = request.cmdObj);
        net->blackHole(noi);
    }
    net->exitNetwork();

    // ElectionState::start is called when election timeout expires, so election
    // finished event has been set.
    getReplCoord()->waitForElectionFinish_forTest();

    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    simulateCatchUpAbort();
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    const auto opCtxPtr = makeOperationContext();
    auto& opCtx = *opCtxPtr;

    // Since we're still in drain mode, expect that we report isWritablePrimary:false,
    // issecondary:true.
    auto helloResponse =
        getReplCoord()->awaitHelloResponse(opCtxPtr.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_TRUE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();
    signalDrainComplete(&opCtx);
    helloResponse =
        getReplCoord()->awaitHelloResponse(opCtxPtr.get(), {}, boost::none, boost::none);
    ASSERT_TRUE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_FALSE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();
}

TEST_F(ReplCoordTest, StartElectionDoesNotStartAnElectionWhenNodeIsRecovering) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT(getReplCoord()->getMemberState().recovering())
        << getReplCoord()->getMemberState().toString();

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(10, 1), 0),
                                                        Date_t() + Seconds(10));
    simulateEnoughHeartbeatsForAllNodesUp();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenNodeIsTheOnlyNode) {
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(10, 1), 0),
                                                        Date_t() + Seconds(10));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->waitForElectionFinish_forTest();
    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    const auto opCtxPtr = makeOperationContext();
    auto& opCtx = *opCtxPtr;

    // Since we're still in drain mode, expect that we report isWritablePrimary:false,
    // issecondary:true.
    auto helloResponse =
        getReplCoord()->awaitHelloResponse(opCtxPtr.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_TRUE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();
    signalDrainComplete(&opCtx);
    helloResponse =
        getReplCoord()->awaitHelloResponse(opCtxPtr.get(), {}, boost::none, boost::none);
    ASSERT_TRUE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_FALSE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();

    // Check that only the 'numCatchUpsSkipped' primary catchup conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtxPtr.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtxPtr.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenAllNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    startCapturingLogMessages();
    simulateSuccessfulV1Election();
    getReplCoord()->waitForElectionFinish_forTest();

    auto opCtxHolder{makeOperationContext()};
    auto* const opCtx{opCtxHolder.get()};

    // Check last vote
    auto lastVote = getExternalState()->loadLocalLastVoteDocument(opCtx);
    ASSERT(lastVote.isOK());
    ASSERT_EQ(0, lastVote.getValue().getCandidateIndex());
    ASSERT_EQ(1, lastVote.getValue().getTerm());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Election succeeded"));

    // Check that the numElectionTimeoutsCalled and the numElectionTimeoutsSuccessful election
    // metrics have been incremented, and that none of the metrics that track the number of
    // elections called or successful for other reasons has been incremented.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsSuccessful_forTesting());
    ASSERT_EQUALS(0,
                  ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsSuccessful_forTesting());
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenMaxSevenNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")
                                           << BSON("_id" << 4 << "host"
                                                         << "node4:12345")
                                           << BSON("_id" << 5 << "host"
                                                         << "node5:12345")
                                           << BSON("_id" << 6 << "host"
                                                         << "node6:12345")
                                           << BSON("_id" << 7 << "host"
                                                         << "node7:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    startCapturingLogMessages();
    simulateSuccessfulV1Election();
    getReplCoord()->waitForElectionFinish_forTest();

    auto opCtxHolder{makeOperationContext()};
    auto* const opCtx{opCtxHolder.get()};

    // Check last vote
    auto lastVote = getExternalState()->loadLocalLastVoteDocument(opCtx);
    ASSERT(lastVote.isOK());
    ASSERT_EQ(0, lastVote.getValue().getCandidateIndex());
    ASSERT_EQ(1, lastVote.getValue().getTerm());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Election succeeded"));
}

// This is to demonstrate the unit testing mock framework. The logic is the same as the original
// test.
TEST_F(ReplCoordMockTest, ElectionFailsWhenInsufficientVotesAreReceivedDuringDryRun_Mock) {
    startCapturingLogMessages();

    // Check that the node's election candidate metrics are unset before it becomes primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(2145400,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    _mock
        ->expect("replSetRequestVotes",
                 BSON("ok" << 1 << "term" << 0 << "voteGranted" << false << "reason"
                           << "don't like him much"))
        .times(2);

    // Trigger election.
    _mock->runUntil(electionTimeoutWhen);
    _mock->runUntilExpectationsSatisfied();

    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, we received insufficient votes"));

    // Check that the node's election candidate metrics have been cleared, since it lost the dry-run
    // election and will not become primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());
}

// This is to showcase the mock's behavior when we have extraneous, unsatisfied expectations.
TEST_F(ReplCoordMockTest,
       ElectionFailsWhenInsufficientVotesAreReceivedDuringDryRun_MockExtraExpectation) {
    startCapturingLogMessages();

    // Check that the node's election candidate metrics are unset before it becomes primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(2145401,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    // This will be satisfied.
    _mock
        ->expect("replSetRequestVotes",
                 BSON("ok" << 1 << "term" << 0 << "voteGranted" << false << "reason"
                           << "don't like him much"))
        .times(2);

    // This will NOT be satisfied.
    _mock->expect("someothercommand", BSON("ok" << 1)).times(42);

    // Trigger election. This will also satisfy our original expectation.
    _mock->runUntil(electionTimeoutWhen);

    // This will throw, as we have an unsatisfied expectation.
    ASSERT_THROWS_CODE(_mock->verifyExpectations(), DBException, (ErrorCodes::Error)5015501);

    // Clear expectations so we can end the test.
    // (You should normally aim to meet all of your expectations.)
    _mock->clearExpectations();

    // Alternatively, calling this first will hang until everything has been satisfied.
    // In our case, this will trivially succeed as we cleared all expectations above.
    _mock->runUntilExpectationsSatisfied();

    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, we received insufficient votes"));

    // Check that the node's election candidate metrics have been cleared, since it lost the dry-run
    // election and will not become primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());
}

TEST_F(ReplCoordTest, ElectionFailsWhenInsufficientVotesAreReceivedDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    // Check that the node's election candidate metrics are unset before it becomes primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(2145402,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    int voteRequests = 0;
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (voteRequests < 2) {
        if (net->now() < electionTimeoutWhen) {
            net->runUntil(electionTimeoutWhen);
        }
        ASSERT_TRUE(net->hasReadyRequests());
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21455,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (consumeHeartbeatV1(noi)) {
            // The heartbeat has been consumed.
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 0 << "voteGranted"
                                                               << false << "reason"
                                                               << "don't like him much")));
            voteRequests++;

            // Check that the node's election candidate metrics are not set if a dry run fails.
            ASSERT_BSONOBJ_EQ(
                BSONObj(),
                ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());
        } else {
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, we received insufficient votes"));

    // Check that the node's election candidate metrics have been cleared, since it lost the dry-run
    // election and will not become primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());
}

TEST_F(ReplCoordTest, ElectionFailsWhenDryRunResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21456,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    int voteRequests = 0;
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (voteRequests < 1) {
        if (net->now() < electionTimeoutWhen) {
            net->runUntil(electionTimeoutWhen);
        }
        ASSERT_TRUE(net->hasReadyRequests());
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21457,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (consumeHeartbeatV1(noi)) {
            // The heartbeat has been consumed.
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long() + 1
                                             << "voteGranted" << false << "reason"
                                             << "quit living in the past")));
            voteRequests++;
        } else {
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, we have been superseded already"));
}

TEST_F(ReplCoordTest, ElectionParticipantMetricsAreCollected) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    OpTime lastOplogEntry = OpTime(Timestamp(999, 0), 1);

    auto metricsAfterVoteRequestWithDryRun = [&](bool dryRun) {
        repl::VoteRequester::Algorithm requester(getReplCoord()->getConfig(),
                                                 1 /* candidateIndex */,
                                                 1 /* term */,
                                                 dryRun,
                                                 lastOplogEntry,
                                                 lastOplogEntry,
                                                 -1 /* primaryIndex */
        );

        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        auto voteRequest = requester.getRequests()[0];
        ReplSetRequestVotesArgs requestVotesArgs;
        ASSERT_OK(requestVotesArgs.initialize(voteRequest.cmdObj));
        ReplSetRequestVotesResponse requestVotesResponse;
        ASSERT_OK(getReplCoord()->processReplSetRequestVotes(
            opCtx, requestVotesArgs, &requestVotesResponse));

        auto electionParticipantMetrics =
            ReplicationMetrics::get(getServiceContext()).getElectionParticipantMetricsBSON();

        LOGV2(4745900,
              "Got election participant metrics",
              "metrics"_attr = electionParticipantMetrics,
              "dryRun"_attr = dryRun);
        return electionParticipantMetrics;
    };

    auto metrics = metricsAfterVoteRequestWithDryRun(true);
    ASSERT_TRUE(metrics.isEmpty());

    metrics = metricsAfterVoteRequestWithDryRun(false);
    ASSERT_TRUE(metrics["votedForCandidate"].Bool());
}

TEST_F(ReplCoordTest, NodeWillNotStandForElectionDuringHeartbeatReconfig) {
    // start up, receive reconfig via heartbeat while at the same time, become candidate.
    // candidate state should be cleared.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 4 << "host"
                                                        << "node4:12345")
                                          << BSON("_id" << 5 << "host"
                                                        << "node5:12345"))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));

    globalFailPointRegistry().find("blockHeartbeatReconfigFinish")->setMode(FailPoint::alwaysOn);

    // hb reconfig
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    ReplSetHeartbeatResponse hbResp2;
    auto config = ReplSetConfig::parse(BSON("_id"
                                            << "mySet"
                                            << "version" << 3 << "members"
                                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                     << "node1:12345")
                                                          << BSON("_id" << 2 << "host"
                                                                        << "node2:12345"))
                                            << "protocolVersion" << 1));
    hbResp2.setConfig(config);
    hbResp2.setConfigVersion(3);
    hbResp2.setSetName("mySet");
    hbResp2.setState(MemberState::RS_SECONDARY);
    hbResp2.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp2.setWrittenOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp2.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    net->runUntil(net->now() + Seconds(10));  // run until we've sent a heartbeat request
    const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
    net->scheduleResponse(noi2, net->now(), makeResponseStatus(hbResp2.toBSON()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();

    // prepare candidacy
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        BSONObjBuilder result;
        ReplicationCoordinator::ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = config.toBSON();
        ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                      getReplCoord()->processReplSetReconfig(opCtx, args, &result));
    }

    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(2)};
    startCapturingLogMessages();

    // receive sufficient heartbeats to allow the node to see a majority.
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    net->enterNetwork();
    for (int i = 0; i < 2; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21458,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            hbResp.setAppliedOpTimeAndWallTime(
                {OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
            hbResp.setWrittenOpTimeAndWallTime(
                {OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
            hbResp.setDurableOpTimeAndWallTime(
                {OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
            BSONObjBuilder respObj;
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
        } else {
            LOGV2_ERROR(21474,
                        "Black holing unexpected request to {request_target}: {request_cmdObj}",
                        "request_target"_attr = request.target,
                        "request_cmdObj"_attr = request.cmdObj);
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    // Advance the simulator clock sufficiently to trigger an election.
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21459,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    net->enterNetwork();
    while (net->now() < electionTimeoutWhen) {
        net->runUntil(electionTimeoutWhen);
        if (!net->hasReadyRequests()) {
            continue;
        }
        auto noi = net->getNextReadyRequest();
        if (!consumeHeartbeatV1(noi)) {
            // Black hole all requests other than heartbeats including vote requests.
            net->blackHole(noi);
        }
    }
    net->exitNetwork();

    stopCapturingLogMessages();
    // ensure node does not stand for election
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Not standing for election; processing "
                                                    "a configuration change"));
    globalFailPointRegistry().find("blockHeartbeatReconfigFinish")->setMode(FailPoint::off);
}

TEST_F(ReplCoordTest, ElectionFailsWhenInsufficientVotesAreReceivedDuringRequestVotes) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21460,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 1 << "voteGranted"
                                                               << false << "reason"
                                                               << "don't like him much")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1,
        countTextFormatLogLinesContaining("Not becoming primary, we received insufficient votes"));
}

TEST_F(ReplCoordTest, TransitionToRollbackFailsWhenElectionInProgress) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    const auto opCtx = makeOperationContext();
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

    ASSERT_EQUALS(ErrorCodes::ElectionInProgress,
                  getReplCoord()->setFollowerModeRollback(opCtx.get()));

    ASSERT_FALSE(getReplCoord()->getMemberState().rollback());

    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the freshness checker and election command runner.
}

TEST_F(ReplCoordTest, ElectionFailsWhenVoteRequestResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    // Check that the node's election candidate metrics are unset before it becomes primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    // Check that the node's election candidate metrics are set once it has called an election.
    ASSERT_BSONOBJ_NE(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21461,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long() + 1
                                             << "voteGranted" << false << "reason"
                                             << "quit living in the past")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1,
        countTextFormatLogLinesContaining("Not becoming primary, we have been superseded already"));

    // Check that the node's election candidate metrics have been cleared, since it lost the actual
    // election and will not become primary.
    ASSERT_BSONOBJ_EQ(
        BSONObj(), ReplicationMetrics::get(getServiceContext()).getElectionCandidateMetricsBSON());
}

TEST_F(ReplCoordTest, ElectionFailsWhenTermChangesDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);

    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    auto onDryRunRequest = [this](const RemoteCommandRequest& request) {
        // Update to a future term before dry run completes.
        ASSERT_EQUALS(0, request.cmdObj.getIntField("candidateIndex"));
        ASSERT(getTopoCoord().updateTerm(1000, getNet()->now()) ==
               TopologyCoordinator::UpdateTermResult::kUpdatedTerm);
    };
    simulateSuccessfulDryRun(onDryRunRequest);

    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, we have been superseded already during dry run"));
}

TEST_F(ReplCoordTest, ElectionFailsWhenTermChangesDuringActualElection) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, Date_t() + Seconds(time1.getSecs()));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};
        // update to a future term before the election completes
        getReplCoord()->updateTerm(opCtx, 1000).transitional_ignore();
    }

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21462,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long()
                                             << "voteGranted" << true << "reason"
                                             << "")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1,
        countTextFormatLogLinesContaining("Not becoming primary, we have been superseded already"));
}

TEST_F(ReplCoordTest, StartElectionOnSingleNodeInitiate) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    BSONObj configObj = configWithMembers(1, 0, BSON_ARRAY(member(1, "node1:12345")));

    // Initialize my last applied, so that the node is electable.
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime({Timestamp(100, 1), 0});

    BSONObjBuilder result;
    ASSERT_OK(getReplCoord()->processReplSetInitiate(opCtx.get(), configObj, &result));
    ASSERT_EQUALS(MemberState::RS_RECOVERING, getReplCoord()->getMemberState().s);
    // Oplog applier attempts to transition the state to Secondary, which triggers
    // election on single node.
    getReplCoord()->finishRecoveryIfEligible(opCtx.get());
    // Run pending election operations on executor.
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, StartElectionOnSingleNodeStartup) {
    BSONObj configObj = configWithMembers(1, 0, BSON_ARRAY(member(1, "node1:12345")));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));

    // Initialize my last applied, so that the node is electable.
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime({Timestamp(100, 1), 0});

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_RECOVERING));
    // Oplog applier attempts to transition the state to Secondary, which triggers
    // election on single node.
    auto opCtx = makeOperationContext();
    getReplCoord()->finishRecoveryIfEligible(opCtx.get());
    // Run pending election operations on executor.
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getReplCoord()->getMemberState().s);
}

class TakeoverTest : public ReplCoordTest {
public:
    /*
     * Verify that a given priority takeover delay is valid. Takeover delays are
     * verified in terms of bounds since the delay value is randomized.
     */
    void assertValidPriorityTakeoverDelay(ReplSetConfig config,
                                          Date_t now,
                                          Date_t priorityTakeoverTime,
                                          int nodeIndex) {

        Milliseconds priorityTakeoverDelay = priorityTakeoverTime - now;
        Milliseconds electionTimeout = config.getElectionTimeoutPeriod();

        long long baseTakeoverDelay =
            durationCount<Milliseconds>(config.getPriorityTakeoverDelay(nodeIndex));
        long long randomOffsetUpperBound = durationCount<Milliseconds>(electionTimeout) *
            getExternalState()->getElectionTimeoutOffsetLimitFraction();

        auto takeoverDelayUpperBound = Milliseconds(baseTakeoverDelay + randomOffsetUpperBound);
        auto takeoverDelayLowerBound = Milliseconds(baseTakeoverDelay);

        ASSERT_GREATER_THAN_OR_EQUALS(priorityTakeoverDelay, takeoverDelayLowerBound);
        ASSERT_LESS_THAN_OR_EQUALS(priorityTakeoverDelay, takeoverDelayUpperBound);
    }

    /*
     * Processes and mocks responses to any pending PV1 heartbeat requests that have been
     * scheduled at or before 'until'. For any such scheduled heartbeat requests, the
     * heartbeat responses will be mocked at the same time the request was made. So,
     * for a heartbeat request made at time 't', the response will be mocked as
     * occurring at time 't'. This function will always run the clock forward to time
     * 'until'.
     *
     * The applied & durable optimes of the mocked response will be set to
     * 'otherNodesOpTime', and the primary set as 'primaryHostAndPort'.
     *
     * Returns the time that it ran until, which should always be equal to 'until'.
     */
    Date_t respondToHeartbeatsUntil(const ReplSetConfig& config,
                                    Date_t until,
                                    const HostAndPort& primaryHostAndPort,
                                    const OpTime& otherNodesOpTime) {

        auto net = getNet();
        net->enterNetwork();

        // If 'until' is equal to net->now(), process any currently queued requests and return,
        // without running the clock.
        if (net->now() == until) {
            _respondToHeartbeatsNow(config, primaryHostAndPort, otherNodesOpTime);
        } else {
            // Otherwise, run clock and process heartbeats along the way.
            while (net->now() < until) {
                // Run clock forward to time 'until', or until the time of the next queued request.
                net->runUntil(until);
                _respondToHeartbeatsNow(config, primaryHostAndPort, otherNodesOpTime);
            }
        }

        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_EQ(net->now(), until);

        return net->now();
    }

    void performSuccessfulTakeover(OperationContext* opCtx,
                                   Date_t takeoverTime,
                                   StartElectionReasonEnum reason,
                                   const LastVote& lastVoteExpected) {
        startCapturingLogMessages();
        simulateSuccessfulV1ElectionAt(opCtx, takeoverTime);
        getReplCoord()->waitForElectionFinish_forTest();
        stopCapturingLogMessages();

        ASSERT(getReplCoord()->getMemberState().primary());

        // Check last vote
        auto lastVote = getExternalState()->loadLocalLastVoteDocument(opCtx);
        ASSERT(lastVote.isOK());
        ASSERT_EQ(lastVoteExpected.getCandidateIndex(), lastVote.getValue().getCandidateIndex());
        ASSERT_EQ(lastVoteExpected.getTerm(), lastVote.getValue().getTerm());

        if (reason == StartElectionReasonEnum::kPriorityTakeover) {
            ASSERT_EQUALS(
                1,
                countTextFormatLogLinesContaining("Starting an election for a priority takeover"));
        }
        ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Election succeeded"));
    }

private:
    /*
     * Processes and schedules mock responses to any PV1 heartbeat requests scheduled at or
     * before the current time. Assumes that the caller has already entered the network with
     * 'enterNetwork()'. It does not run the virtual clock.
     *
     * Intended as a helper function only.
     */
    void _respondToHeartbeatsNow(const ReplSetConfig& config,
                                 const HostAndPort& primaryHostAndPort,
                                 const OpTime& otherNodesOpTime) {

        auto replCoord = getReplCoord();
        auto net = getNet();

        // Process all requests queued at the present time.
        while (net->hasReadyRequests()) {

            // If we see that the next request isn't for a heartbeat, exit the function.
            // This allows us to mock heartbeat responses with whatever info we want
            // right up until another event happens (like an election). This is
            // particularly important for simulating a catchup takeover because
            // we need to know specific info about the primary.
            auto noi = net->getFrontOfUnscheduledQueue();
            auto&& nextRequest = noi->getRequest();
            if (nextRequest.cmdObj.firstElement().fieldNameStringData() != "replSetHeartbeat") {
                return;
            }

            noi = net->getNextReadyRequest();
            auto&& request = noi->getRequest();

            LOGV2(21463,
                  "{request_target} processing {request_cmdObj} at {net_now}",
                  "request_target"_attr = request.target,
                  "request_cmdObj"_attr = request.cmdObj,
                  "net_now"_attr = net->now());

            // Make sure the heartbeat request is valid.
            ReplSetHeartbeatArgsV1 hbArgs;
            ASSERT_OK(hbArgs.initialize(request.cmdObj));

            // Build the mock heartbeat response.
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(config.getReplSetName());
            if (request.target == primaryHostAndPort) {
                hbResp.setState(MemberState::RS_PRIMARY);
            } else {
                hbResp.setState(MemberState::RS_SECONDARY);
            }
            hbResp.setConfigVersion(config.getConfigVersion());
            hbResp.setTerm(replCoord->getTerm());
            hbResp.setAppliedOpTimeAndWallTime(
                {otherNodesOpTime, Date_t() + Seconds(otherNodesOpTime.getSecs())});
            hbResp.setWrittenOpTimeAndWallTime(
                {otherNodesOpTime, Date_t() + Seconds(otherNodesOpTime.getSecs())});
            hbResp.setDurableOpTimeAndWallTime(
                {otherNodesOpTime, Date_t() + Seconds(otherNodesOpTime.getSecs())});
            auto response = makeResponseStatus(hbResp.toBSON());
            net->scheduleResponse(noi, net->now(), response);
        }
    }
};

TEST_F(TakeoverTest, DoesntScheduleCatchupTakeoverIfCatchupDisabledButTakeoverDelaySet) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1 << "settings"
                             << BSON("catchUpTimeoutMillis" << 0 << "catchUpTakeoverDelay"
                                                            << 10000));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        ASSERT_EQUALS(ErrorCodes::StaleTerm, replCoord->updateTerm(opCtx, 1));
    }

    // Make sure we're secondary and that no catchup takeover has been scheduled yet.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough
    // information to know that we are fresher than the current primary.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover was not scheduled.
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());
}

TEST_F(TakeoverTest, SchedulesCatchupTakeoverIfNodeIsFresherThanCurrentPrimary) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled yet.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);
}

TEST_F(TakeoverTest, SchedulesCatchupTakeoverIfBothTakeoversAnOption) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"
                                                         << "priority" << 3))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled at the
    // correct time and that a priority takeover has not been scheduled.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);
}

TEST_F(TakeoverTest, PrefersPriorityToCatchupTakeoverIfNodeHasHighestPriority) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);

    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(2)};
    startCapturingLogMessages();

    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Assert that a priority takeover has been scheduled and that a catchup takeover has not
    // been scheduled.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "I can take over the primary because I have a higher priority, "
                      "the highest priority in the replica set, and fresher data"));
}

TEST_F(TakeoverTest, CatchupTakeoverNotScheduledTwice) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    executor::TaskExecutor::CallbackHandle catchupTakeoverCbh =
        replCoord->getCatchupTakeoverCbh_forTest();
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // Mock another round of heartbeat responses
    now = respondToHeartbeatsUntil(
        config, now + config.getHeartbeatInterval(), HostAndPort("node2", 12345), behindOptime);

    // Make sure another catchup takeover wasn't scheduled
    ASSERT_EQUALS(catchupTakeoverTime, replCoord->getCatchupTakeover_forTest().value());
    ASSERT_TRUE(catchupTakeoverCbh == replCoord->getCatchupTakeoverCbh_forTest());
}

TEST_F(TakeoverTest, CatchupAndPriorityTakeoverNotScheduledAtSameTime) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"
                                                         << "priority" << 3))
                             << "protocolVersion" << 1);
    // In order for node 1 to first schedule a catchup takeover, then a priority takeover
    // once the first gets canceled, it must have a higher priority than the current primary
    // (node 2). But, it must not have the highest priority in the replica set. Otherwise,
    // it will schedule a priority takeover from the start.
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // Create a new OpTime so that the primary's last applied OpTime will be in the current term.
    OpTime caughtupOptime(Timestamp(300, 1), 1);
    // Mock another heartbeat where the primary is now up to date.
    now = respondToHeartbeatsUntil(
        config, now + catchupTakeoverDelay / 2, HostAndPort("node2", 12345), caughtupOptime);

    // Since the primary has caught up, we cancel the scheduled catchup takeover.
    // But we are still higher priority than the primary, so after the heartbeat
    // we will schedule a priority takeover.
    ASSERT(replCoord->getPriorityTakeover_forTest());
}

TEST_F(TakeoverTest, CatchupTakeoverCallbackCanceledIfElectionTimeoutRuns) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    // Force election timeouts to be exact, with no randomized offset, so that when the election
    // timeout fires below we still think we can see a majority.
    getExternalState()->setElectionTimeoutOffsetLimitFraction(0);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    startCapturingLogMessages();

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // Fast forward clock to after electionTimeout and black hole all
    // heartbeat requests to make sure the election timeout runs.
    Date_t electionTimeout = replCoord->getElectionTimeout_forTest();
    auto net = getNet();
    net->enterNetwork();
    while (net->now() < electionTimeout) {
        net->runUntil(electionTimeout);
        while (net->hasReadyRequests()) {
            auto noi = net->getNextReadyRequest();
            net->blackHole(noi);
        }
    }
    ASSERT_EQUALS(electionTimeout, net->now());
    net->exitNetwork();

    stopCapturingLogMessages();

    ASSERT_EQUALS(
        1, countTextFormatLogLinesContaining("Starting an election, since we've seen no PRIMARY"));

    // Make sure catchup takeover never happend and CatchupTakeover callback was canceled.
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());
    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Canceling catchup takeover callback"));
    ASSERT_EQUALS(0,
                  countTextFormatLogLinesContaining("Starting an election for a catchup takeover"));
}

TEST_F(TakeoverTest, CatchupTakeoverCanceledIfTransitionToRollback) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    // Update the current term to simulate a scenario where an election has occured
    // and some other node became the new primary. Once you hear about a primary election
    // in term 1, your term will be increased.
    replCoord->updateTerm_forTest(1, nullptr);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    startCapturingLogMessages();

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    const auto opCtx = makeOperationContext();
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

    // Transitioning to rollback state should cancel the takeover
    ASSERT_OK(replCoord->setFollowerModeRollback(opCtx.get()));
    ASSERT_TRUE(replCoord->getMemberState().rollback());

    stopCapturingLogMessages();

    // Make sure catchup takeover never happend and CatchupTakeover callback was canceled.
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Canceling catchup takeover callback"));
    ASSERT_EQUALS(0,
                  countTextFormatLogLinesContaining("Starting an election for a catchup takeover"));
}

TEST_F(TakeoverTest, SuccessfulCatchupTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(100, 5000), 0);
    OpTime behindOptime(Timestamp(100, 4000), 0);

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));

    // Update the term so that the current term is ahead of the term of
    // the last applied op time. This means that the primary is still in
    // catchup mode since it hasn't written anything this term.
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        ASSERT_EQUALS(ErrorCodes::StaleTerm,
                      replCoord->updateTerm(opCtx, replCoord->getTerm() + 1));
    }

    // Make sure we're secondary and that no takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses.
    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    startCapturingLogMessages();

    // The catchup takeover will be scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock a few rounds of
    // heartbeat responses to prevent a normal election timeout.
    now = respondToHeartbeatsUntil(
        config, catchupTakeoverTime, HostAndPort("node2", 12345), behindOptime);
    stopCapturingLogMessages();

    // Since the heartbeats go through the catchupTakeoverTimeout, this log
    // message happens already (otherwise it would happen in performSuccessfulTakeover).
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Starting an election for a catchup takeover"));

    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        LastVote lastVoteExpected = LastVote(replCoord->getTerm() + 1, 0);
        performSuccessfulTakeover(opCtx,
                                  catchupTakeoverTime,
                                  StartElectionReasonEnum::kCatchupTakeover,
                                  lastVoteExpected);
    }

    // Check that the numCatchUpTakeoversCalled and the numCatchUpTakeoversSuccessful election
    // metrics have been incremented, and that none of the metrics that track the number of
    // elections called or successful for other reasons has been incremented.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsSuccessful_forTesting());
    ASSERT_EQUALS(0,
                  ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsSuccessful_forTesting());
}

TEST_F(TakeoverTest, CatchupTakeoverDryRunFailsPrimarySaysNo) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")
                                           << BSON("_id" << 4 << "host"
                                                         << "node4:12345")
                                           << BSON("_id" << 5 << "host"
                                                         << "node5:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(100, 5000), 0);
    OpTime behindOptime(Timestamp(100, 4000), 0);

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));

    // Update the term so that the current term is ahead of the term of
    // the last applied op time. This means that the primary is still in
    // catchup mode since it hasn't written anything this term.
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        ASSERT_EQUALS(ErrorCodes::StaleTerm,
                      replCoord->updateTerm(opCtx, replCoord->getTerm() + 1));
    }

    // Make sure we're secondary and that no takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    // Mock a first round of heartbeat responses.
    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // The catchup takeover will be scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock a few rounds of
    // heartbeat responses to prevent a normal election timeout.
    now = respondToHeartbeatsUntil(
        config, catchupTakeoverTime, HostAndPort("node2", 12345), behindOptime);

    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Starting an election for a catchup takeover"));

    // Simulate a dry run where the primary has caught up and is now ahead of the
    // node trying to do the catchup takeover. All the secondary nodes respond
    // first so that it tests that we require the primary vote even when we've
    // received a majority of the votes. Then the primary responds no to the vote
    // request and as a result the dry run fails.
    int voteRequests = 0;
    int votesExpected = config.getNumMembers() - 1;
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi_primary;
    Date_t until = net->now() + Seconds(1);
    while (voteRequests < votesExpected) {
        LOGV2(21464,
              "request: {voteRequests} expected: {votesExpected}",
              "voteRequests"_attr = voteRequests,
              "votesExpected"_attr = votesExpected);
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21465,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            bool voteGranted = request.target != primaryHostAndPort;
            net->scheduleResponse(noi,
                                  until,
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 1 << "voteGranted"
                                                               << voteGranted << "reason"
                                                               << "")));
            voteRequests++;
        }
        net->runReadyNetworkOperations();
    }

    while (net->now() < until) {
        net->runUntil(until);
        if (net->hasReadyRequests()) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            net->blackHole(noi);
        }
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionDryRunFinish_forTest();
    stopCapturingLogMessages();

    // Make sure an election wasn't called for and that we are still secondary.
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Not running for primary, the current primary responded no in the dry run"));
    ASSERT(replCoord->getMemberState().secondary());
}

TEST_F(TakeoverTest, PrimaryCatchesUpBeforeCatchupTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Update the term so that the current term is ahead of the term of
    // the last applied op time.
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        ASSERT_EQUALS(ErrorCodes::StaleTerm,
                      replCoord->updateTerm(opCtx, replCoord->getTerm() + 1));
    }

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    startCapturingLogMessages();

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // Mock another heartbeat where the primary is now up to date
    // and run time through when catchup takeover was supposed to happen.
    now = respondToHeartbeatsUntil(
        config, now + catchupTakeoverDelay, HostAndPort("node2", 12345), currentOptime);

    stopCapturingLogMessages();

    // Make sure we're secondary and that no catchup takeover election happened.
    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());
    ASSERT_EQUALS(
        1, countTextFormatLogLinesContaining("Not starting an election for a catchup takeover"));
}

TEST_F(TakeoverTest, PrimaryCatchesUpBeforeHighPriorityNodeCatchupTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"
                                                         << "priority" << 3))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOptime(Timestamp(200, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        currentOptime, Date_t() + Seconds(currentOptime.getSecs()));
    OpTime behindOptime(Timestamp(100, 1), 0);

    // Update the term so that the current term is ahead of the term of
    // the last applied op time.
    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        ASSERT_EQUALS(ErrorCodes::StaleTerm,
                      replCoord->updateTerm(opCtx, replCoord->getTerm() + 1));
    }

    // Make sure we're secondary and that no catchup takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());

    startCapturingLogMessages();

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we are fresher than the current primary, prompting the scheduling of a catchup
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), behindOptime);

    // Make sure that the catchup takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getCatchupTakeover_forTest());
    auto catchupTakeoverTime = replCoord->getCatchupTakeover_forTest().value();
    Milliseconds catchupTakeoverDelay = catchupTakeoverTime - now;
    ASSERT_EQUALS(config.getCatchUpTakeoverDelay(), catchupTakeoverDelay);

    // Mock another heartbeat where the primary is now up to date
    // and run time through when catchup takeover was supposed to happen.
    now = respondToHeartbeatsUntil(
        config, now + catchupTakeoverDelay, HostAndPort("node2", 12345), currentOptime);

    stopCapturingLogMessages();

    // Make sure we're secondary and that no catchup takeover election happens.
    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_FALSE(replCoord->getCatchupTakeover_forTest());
    ASSERT_EQUALS(
        1, countTextFormatLogLinesContaining("Not starting an election for a catchup takeover"));

    // Make sure that the priority takeover has now been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Node 1 schedules the priority takeover, and since it has the second highest
    // priority in the replica set, it will schedule in 20 seconds. We must increase
    // the election timeout so that the priority takeover will actually be executed.
    // Mock another round of heartbeat responses to prevent a normal election timeout.
    Milliseconds longElectionTimeout = config.getElectionTimeoutPeriod() * 2;
    now = respondToHeartbeatsUntil(
        config, now + longElectionTimeout, HostAndPort("node2", 12345), currentOptime);

    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        LastVote lastVoteExpected = LastVote(replCoord->getTerm() + 1, 0);
        performSuccessfulTakeover(opCtx,
                                  priorityTakeoverTime,
                                  StartElectionReasonEnum::kPriorityTakeover,
                                  lastVoteExpected);
    }
}

TEST_F(TakeoverTest, SchedulesPriorityTakeoverIfNodeHasHigherPriorityThanCurrentPrimary) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime myOptime(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(myOptime,
                                                        Date_t() + Seconds(myOptime.getSecs()));

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we supersede priorities of all other nodes, prompting the scheduling of a priority
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), myOptime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Also make sure that updating the term cancels the scheduled priority takeover.
    {
        const auto opCtxPtr = makeOperationContext();
        auto& opCtx = *opCtxPtr;

        ASSERT_EQUALS(ErrorCodes::StaleTerm,
                      replCoord->updateTerm(&opCtx, replCoord->getTerm() + 1));
    }
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());
}

TEST_F(TakeoverTest, SuccessfulPriorityTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime myOptime(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(myOptime,
                                                        Date_t() + Seconds(myOptime.getSecs()));

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we supersede priorities of all other nodes, prompting the scheduling of a priority
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), myOptime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // The priority takeover might be scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock another round of
    // heartbeat responses to prevent a normal election timeout.
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, now + halfElectionTimeout, HostAndPort("node2", 12345), myOptime);

    {
        const auto opCtxPtr = makeOperationContext();
        auto& opCtx = *opCtxPtr;

        LastVote lastVoteExpected = LastVote(replCoord->getTerm() + 1, 0);
        performSuccessfulTakeover(&opCtx,
                                  priorityTakeoverTime,
                                  StartElectionReasonEnum::kPriorityTakeover,
                                  lastVoteExpected);
    }

    // Check that the numPriorityTakeoversCalled and the numPriorityTakeoversSuccessful election
    // metrics have been incremented, and that none of the metrics that track the number of
    // elections called or successful for other reasons has been incremented.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsSuccessful_forTesting());
    ASSERT_EQUALS(1,
                  ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsSuccessful_forTesting());
}

TEST_F(TakeoverTest, DontCallForPriorityTakeoverWhenLaggedSameSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOpTime(Timestamp(100, 5000), 0);
    OpTime behindOpTime(Timestamp(100, 3999), 0);
    OpTime closeEnoughOpTime(Timestamp(100, 4000), 0);

    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(behindOpTime,
                                                        Date_t() + Seconds(behindOpTime.getSecs()));

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses.
    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, currentOpTime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    now = respondToHeartbeatsUntil(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();

    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(
            BSON("attr" << BSON(
                     "reason"
                     << "Not standing for election because member is not "
                        "caught up enough to the most up-to-date member to "
                        "call for priority takeover - must be within 2 seconds (mask 0x80)"))));

    // Mock another round of heartbeat responses that occur after the previous
    // 'priorityTakeoverTime', which should schedule a new priority takeover
    Milliseconds heartbeatInterval = config.getHeartbeatInterval() / 4;
    // Run clock forward to the time of the next queued heartbeat request.
    getNet()->enterNetwork();
    getNet()->runUntil(now + heartbeatInterval);
    getNet()->exitNetwork();
    now = respondToHeartbeatsUntil(config, getNet()->now(), primaryHostAndPort, currentOpTime);

    // Make sure that a new priority takeover has been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        closeEnoughOpTime, Date_t() + Seconds(closeEnoughOpTime.getSecs()));

    // The priority takeover might have been scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock another round of
    // heartbeat responses to prevent a normal election timeout.
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, now + halfElectionTimeout, primaryHostAndPort, currentOpTime);

    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        LastVote lastVoteExpected = LastVote(replCoord->getTerm() + 1, 0);
        performSuccessfulTakeover(opCtx,
                                  priorityTakeoverTime,
                                  StartElectionReasonEnum::kPriorityTakeover,
                                  lastVoteExpected);
    }
}

TEST_F(TakeoverTest, DontCallForPriorityTakeoverWhenLaggedDifferentSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OpTime currentOpTime(Timestamp(100, 1), 0);
    OpTime behindOpTime(Timestamp(97, 1), 0);
    OpTime closeEnoughOpTime(Timestamp(98, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(behindOpTime,
                                                        Date_t() + Seconds(behindOpTime.getSecs()));

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());


    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, currentOpTime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    now = respondToHeartbeatsUntil(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();

    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(
            BSON("attr" << BSON(
                     "reason"
                     << "Not standing for election because member is not "
                        "caught up enough to the most up-to-date member to "
                        "call for priority takeover - must be within 2 seconds (mask 0x80)"))));

    // Mock another round of heartbeat responses that occur after the previous
    // 'priorityTakeoverTime', which should schedule a new priority takeover
    Milliseconds heartbeatInterval = config.getHeartbeatInterval() / 4;
    // Run clock forward to the time of the next queued heartbeat request.
    getNet()->enterNetwork();
    getNet()->runUntil(now + heartbeatInterval);
    getNet()->exitNetwork();
    now = respondToHeartbeatsUntil(config, getNet()->now(), primaryHostAndPort, currentOpTime);

    // Make sure that a new priority takeover has been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().value();
    assertValidPriorityTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(
        closeEnoughOpTime, Date_t() + Seconds(closeEnoughOpTime.getSecs()));

    // The priority takeover might have been scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock another round of
    // heartbeat responses to prevent a normal election timeout.
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, now + halfElectionTimeout, primaryHostAndPort, currentOpTime);

    {
        auto opCtxHolder{makeOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        LastVote lastVoteExpected = LastVote(replCoord->getTerm() + 1, 0);
        performSuccessfulTakeover(opCtx,
                                  priorityTakeoverTime,
                                  StartElectionReasonEnum::kPriorityTakeover,
                                  lastVoteExpected);
    }
}

TEST_F(ReplCoordTest, NodeCancelsElectionUponReceivingANewConfigDuringDryRun) {
    // Start up and become electable.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "protocolVersion" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings" << BSON("heartbeatIntervalMillis" << 100)),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    simulateEnoughHeartbeatsForAllNodesUp();

    // Advance to dry run vote request phase.
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (TopologyCoordinator::Role::kCandidate != getTopoCoord().getRole()) {
        net->runUntil(net->now() + Seconds(1));
        if (!net->hasReadyRequests()) {
            continue;
        }
        auto noi = net->getNextReadyRequest();
        // Consume the heartbeat or black hole it.
        if (!consumeHeartbeatV1(noi)) {
            net->blackHole(noi);
        }
    }
    net->exitNetwork();
    ASSERT(TopologyCoordinator::Role::kCandidate == getTopoCoord().getRole());

    // Submit a reconfig and confirm it cancels the election.
    ReplicationCoordinatorImpl::ReplSetReconfigArgs config = {
        BSON("_id"
             << "mySet"
             << "version" << 4 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345"))),
        true};

    BSONObjBuilder result;
    const auto opCtx = makeOperationContext();
    ASSERT_OK(getReplCoord()->processReplSetReconfig(opCtx.get(), config, &result));
    // Wait until election cancels.
    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();
    ASSERT(TopologyCoordinator::Role::kFollower == getTopoCoord().getRole());
}

TEST_F(ReplCoordTest, NodeCancelsElectionUponReceivingANewConfigDuringVotePhase) {
    // Start up and become electable.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "protocolVersion" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings" << BSON("heartbeatIntervalMillis" << 100)),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();
    ASSERT(TopologyCoordinator::Role::kCandidate == getTopoCoord().getRole());

    // Submit a reconfig and confirm it cancels the election.
    ReplicationCoordinatorImpl::ReplSetReconfigArgs config = {
        BSON("_id"
             << "mySet"
             << "version" << 4 << "protocolVersion" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345"))),
        true};

    BSONObjBuilder result;
    const auto opCtx = makeOperationContext();
    ASSERT_OK(getReplCoord()->processReplSetReconfig(opCtx.get(), config, &result));
    // Wait until election cancels.
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    ASSERT(TopologyCoordinator::Role::kFollower == getTopoCoord().getRole());
}

TEST_F(ReplCoordTest, NodeCancelsElectionWhenWritingLastVoteInDryRun) {
    // Start up and become electable.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "protocolVersion" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings" << BSON("heartbeatIntervalMillis" << 100)),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    simulateEnoughHeartbeatsForAllNodesUp();
    // Set a failpoint to hang after checking the results for the dry run but before we initiate the
    // vote request for the real election.
    const auto hangInWritingLastVoteForDryRun =
        globalFailPointRegistry().find("hangInWritingLastVoteForDryRun");
    const auto timesEnteredFailPoint = hangInWritingLastVoteForDryRun->setMode(FailPoint::alwaysOn);
    stdx::thread electionThread([&] { simulateSuccessfulDryRun(); });
    // Wait to hit the failpoint.
    hangInWritingLastVoteForDryRun->waitForTimesEntered(timesEnteredFailPoint + 1);
    ASSERT(TopologyCoordinator::Role::kCandidate == getTopoCoord().getRole());

    // Cancel the election after the dry-run has already completed but before we create a new
    // VoteRequester.
    startCapturingLogMessages();
    getReplCoord()->cancelElection_forTest();
    hangInWritingLastVoteForDryRun->setMode(FailPoint::off, 0);
    electionThread.join();

    // Finish the election. We will receive the requested votes, but the election should still be
    // canceled.
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(4825602,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(
                    BSON("ok" << 1 << "term" << 1 << "voteGranted" << true << "reason"
                              << "The election should be canceled even if I give you votes")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countTextFormatLogLinesContaining("Not becoming primary, election has been cancelled"));
    ASSERT(TopologyCoordinator::Role::kFollower == getTopoCoord().getRole());
}

TEST_F(ReplCoordTest, MemberHbDataIsRestartedUponWinningElection) {
    auto replAllSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(3)};

    auto replConfigBson = BSON("_id"
                               << "mySet"
                               << "protocolVersion" << 1 << "version" << 1 << "members"
                               << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                        << "node1:12345")
                                             << BSON("_id" << 2 << "host"
                                                           << "node2:12345")
                                             << BSON("_id" << 3 << "host"
                                                           << "node3:12345")));

    auto myHostAndPort = HostAndPort("node1", 12345);
    assertStartSuccess(replConfigBson, myHostAndPort);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    // Respond to heartbeat requests. This should update memberData for all nodes.
    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    // Verify that all other nodes were updated since restart.
    auto memberData = getReplCoord()->getMemberData();
    for (auto& member : memberData) {
        // We should not have updated our own memberData.
        if (member.isSelf()) {
            continue;
        }
        ASSERT_TRUE(member.isUpdatedSinceRestart());
    }

    enterNetwork();
    NetworkInterfaceMock* net = getNet();

    // Advance clock time so that heartbeat requests are in SENT state.
    auto config = getReplCoord()->getReplicaSetConfig_forTest();
    net->advanceTime(net->now() + config.getHeartbeatInterval());

    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(5031801,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            // Since the heartbeat requests are in SENT state, we will black hole them here to avoid
            // responding to them.
            net->blackHole(noi);
        } else {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 1 << "voteGranted"
                                                               << true << "reason"
                                                               << "")));
        }
        net->runReadyNetworkOperations();
    }
    exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Verify that the memberData for every node has not been updated since becoming primary. This
    // can only happen if all heartbeat requests are restarted after the election, not just
    // heartbeats in SCHEDULED state.
    memberData = getReplCoord()->getMemberData();
    for (auto& member : memberData) {
        ASSERT_FALSE(member.isUpdatedSinceRestart());
    }
}

class PrimaryCatchUpTest : public ReplCoordTest {
protected:
    using NetworkOpIter = NetworkInterfaceMock::NetworkOperationIterator;
    using NetworkRequestFn = std::function<void(const NetworkOpIter)>;

    const Timestamp smallTimestamp{1, 1};

    executor::RemoteCommandResponse makeHeartbeatResponse(OpTime opTime) {
        ReplSetConfig rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(rsConfig.getReplSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
        hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
        hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
        return makeResponseStatus(hbResp.toBSON());
    }

    void simulateSuccessfulV1Voting() {
        ReplicationCoordinatorImpl* replCoord = getReplCoord();
        NetworkInterfaceMock* net = getNet();

        auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
        ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
        LOGV2(21466,
              "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
              "electionTimeoutWhen"_attr = electionTimeoutWhen);

        ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
        // Process requests until we're primary but leave the heartbeats for the notification
        // of election win. Exit immediately on unexpected requests.
        while (!replCoord->getMemberState().primary()) {
            LOGV2(21467,
                  "Waiting on network in state {replCoord_getMemberState}",
                  "replCoord_getMemberState"_attr = replCoord->getMemberState());
            net->enterNetwork();
            if (net->now() < electionTimeoutWhen) {
                net->runUntil(electionTimeoutWhen);
            }
            // Peek the next request, don't consume it yet.
            const NetworkOpIter noi = net->getFrontOfUnscheduledQueue();
            const RemoteCommandRequest& request = noi->getRequest();
            LOGV2(21468,
                  "{request_target} processing {request_cmdObj}",
                  "request_target"_attr = request.target.toString(),
                  "request_cmdObj"_attr = request.cmdObj);
            if (ReplSetHeartbeatArgsV1().initialize(request.cmdObj).isOK()) {
                OpTime opTime(Timestamp(), getReplCoord()->getTerm());
                net->scheduleResponse(
                    net->getNextReadyRequest(), net->now(), makeHeartbeatResponse(opTime));
            } else if (request.cmdObj.firstElement().fieldNameStringData() ==
                       "replSetRequestVotes") {
                net->scheduleResponse(
                    net->getNextReadyRequest(),
                    net->now(),
                    makeResponseStatus(BSON("ok" << 1 << "reason"
                                                 << ""
                                                 << "term" << request.cmdObj["term"].Long()
                                                 << "voteGranted" << true)));
            } else {
                // Stop the loop and let the caller handle unexpected requests.
                net->exitNetwork();
                break;
            }
            net->runReadyNetworkOperations();
            net->exitNetwork();
        }
    }

    ReplSetConfig setUp3NodeReplSetAndRunForElection(OpTime opTime, long long timeout = 5000) {
        BSONObj configObj = BSON("_id"
                                 << "mySet"
                                 << "version" << 1 << "members"
                                 << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                          << "node1:12345")
                                               << BSON("_id" << 2 << "host"
                                                             << "node2:12345")
                                               << BSON("_id" << 3 << "host"
                                                             << "node3:12345"))
                                 << "protocolVersion" << 1 << "settings"
                                 << BSON("heartbeatTimeoutSecs" << 1 << "catchUpTimeoutMillis"
                                                                << timeout));
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplSetConfig config = assertMakeRSConfig(configObj);

        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(opTime,
                                                            Date_t() + Seconds(opTime.getSecs()));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        simulateSuccessfulV1Voting();
        const auto opCtx = makeOperationContext();
        auto helloResponse =
            getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
        ASSERT_FALSE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
        ASSERT_TRUE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();

        return config;
    }

    executor::RemoteCommandResponse makeFreshnessScanResponse(OpTime opTime) {
        // OpTime part of replSetGetStatus.
        return makeResponseStatus(BSON("optimes" << BSON("appliedOpTime" << opTime)));
    }

    void processHeartbeatRequests(NetworkRequestFn onHeartbeatRequest) {
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        while (net->hasReadyRequests()) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const RemoteCommandRequest& request = noi->getRequest();
            LOGV2(21469,
                  "{request_target} processing heartbeat {request_cmdObj} at {net_now}",
                  "request_target"_attr = request.target.toString(),
                  "request_cmdObj"_attr = request.cmdObj,
                  "net_now"_attr = net->now());
            if (ReplSetHeartbeatArgsV1().initialize(request.cmdObj).isOK()) {
                onHeartbeatRequest(noi);
            } else {
                LOGV2(21470,
                      "Black holing unexpected request to {request_target}: {request_cmdObj}",
                      "request_target"_attr = request.target,
                      "request_cmdObj"_attr = request.cmdObj);
                net->blackHole(noi);
            }
            net->runReadyNetworkOperations();
        }
        net->exitNetwork();
    }

    // Response heartbeats with opTime until the given time. Exit if it sees any other request.
    void replyHeartbeatsAndRunUntil(Date_t until, NetworkRequestFn onHeartbeatRequest) {
        auto net = getNet();
        net->enterNetwork();
        while (net->now() < until) {
            while (net->hasReadyRequests()) {
                // Peek the next request
                auto noi = net->getFrontOfUnscheduledQueue();
                auto& request = noi->getRequest();
                LOGV2(21471,
                      "{request_target} at {net_now} processing {request_cmdObj}",
                      "request_target"_attr = request.target,
                      "net_now"_attr = net->now(),
                      "request_cmdObj"_attr = request.cmdObj);
                if (ReplSetHeartbeatArgsV1().initialize(request.cmdObj).isOK()) {
                    // Consume the next request
                    onHeartbeatRequest(net->getNextReadyRequest());
                } else {
                    // Cannot consume other requests than heartbeats.
                    net->exitNetwork();
                    return;
                }
            }
            net->runUntil(until);
        }
        net->exitNetwork();
    }

    // Simulate the work done by bgsync and applier threads. setMyLastAppliedOpTime() will signal
    // the optime waiter.
    void advanceMyLastWrittenAndAppliedOpTime(OpTime opTime, Date_t wallTime = Date_t()) {
        replCoordSetMyLastWrittenOpTime(opTime, wallTime);
        replCoordSetMyLastAppliedOpTime(opTime, wallTime);
        getNet()->enterNetwork();
        getNet()->runReadyNetworkOperations();
        getNet()->exitNetwork();
    }
};

// The first round of heartbeats indicates we are the most up-to-date.
TEST_F(PrimaryCatchUpTest, PrimaryDoesNotNeedToCatchUp) {
    startCapturingLogMessages();
    OpTime time1(Timestamp(100, 1), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);

    int count = 0;
    processHeartbeatRequests([this, time1, &count](const NetworkOpIter noi) {
        count++;
        auto net = getNet();
        // The old primary accepted one more op and all nodes caught up after voting for me.
        net->scheduleResponse(noi, net->now(), makeHeartbeatResponse(time1));
    });

    // Get 2 heartbeats from secondaries.
    ASSERT_EQUALS(2, count);
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(
        1,
        countTextFormatLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsAlreadyCaughtUp' primary catchup conclusion reason was
    // incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());

    // Check that the targetCatchupOpTime metric was not set.
    ASSERT_EQUALS(boost::none,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());
}

// Heartbeats set a future target OpTime and we reached that successfully.
TEST_F(PrimaryCatchUpTest, CatchupSucceeds) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);

    // Check that the targetCatchupOpTime metric is unset before the target opTime for catchup is
    // set.
    ASSERT_EQUALS(boost::none,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());

    processHeartbeatRequests([this, time2](const NetworkOpIter noi) {
        auto net = getNet();
        // The old primary accepted one more op and all nodes caught up after voting for me.
        net->scheduleResponse(noi, net->now(), makeHeartbeatResponse(time2));
    });

    // Check that the targetCatchupOpTime metric was set correctly when heartbeats updated the
    // target opTime for catchup.
    ASSERT_EQUALS(time2,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());

    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    advanceMyLastWrittenAndAppliedOpTime(time2, Date_t() + Seconds(time2.getSecs()));
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countTextFormatLogLinesContaining("Caught up to the latest known optime successfully"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsSucceeded' primary catchup conclusion reason was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, CatchupTimeout) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    auto catchupTimeoutTime = getNet()->now() + config.getCatchUpTimeoutPeriod();
    replyHeartbeatsAndRunUntil(catchupTimeoutTime, [this, time2](const NetworkOpIter noi) {
        // Other nodes are ahead of me.
        getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time2));
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsTimedOut' primary catchup conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, CannotSeeAllNodes) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    // We should get caught up by the timeout time.
    auto catchupTimeoutTime = getNet()->now() + config.getCatchUpTimeoutPeriod();
    replyHeartbeatsAndRunUntil(catchupTimeoutTime, [this, time1](const NetworkOpIter noi) {
        const RemoteCommandRequest& request = noi->getRequest();
        if (request.target.host() == "node2") {
            auto status = Status(ErrorCodes::HostUnreachable, "Can't reach remote host");
            getNet()->scheduleResponse(noi, getNet()->now(), status);
        } else {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time1));
        }
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(
        1,
        countTextFormatLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsAlreadyCaughtUp' primary catchup conclusion reason was
    // incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, HeartbeatTimeout) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    // We should get caught up by the timeout time.
    auto catchupTimeoutTime = getNet()->now() + config.getCatchUpTimeoutPeriod();
    replyHeartbeatsAndRunUntil(catchupTimeoutTime, [this, time1](const NetworkOpIter noi) {
        const RemoteCommandRequest& request = noi->getRequest();
        if (request.target.host() == "node2") {
            LOGV2(21472,
                  "Black holing heartbeat from {request_target_host}",
                  "request_target_host"_attr = request.target.host());
            getNet()->blackHole(noi);
        } else {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time1));
        }
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(
        1,
        countTextFormatLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsAlreadyCaughtUp' primary catchup conclusion reason was
    // incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, PrimaryStepsDownBeforeHeartbeatRefreshing) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    // Step down immediately.
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    TopologyCoordinator::UpdateTermResult updateTermResult;
    auto evh = getReplCoord()->updateTerm_forTest(2, &updateTermResult);
    ASSERT_TRUE(evh.isValid());
    getReplExec()->waitForEvent(evh);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Since the primary stepped down in catchup mode because it saw a higher term, check that only
    // the 'numCatchUpsFailedWithNewTerm' primary catchup conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, PrimaryStepsDownDuringCatchUp) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    // Step down in the middle of catchup.
    auto abortTime = getNet()->now() + config.getCatchUpTimeoutPeriod() / 2;
    replyHeartbeatsAndRunUntil(abortTime, [this, time2](const NetworkOpIter noi) {
        // Other nodes are ahead of me.
        getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time2));
    });

    ASSERT_EQUALS(time2,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());

    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    TopologyCoordinator::UpdateTermResult updateTermResult;
    auto evh = getReplCoord()->updateTerm_forTest(2, &updateTermResult);
    ASSERT_TRUE(evh.isValid());
    getReplExec()->waitForEvent(evh);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    //    replyHeartbeatsAndRunUntil(getNet()->now() + config.getCatchUpTimeoutPeriod());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Since the primary stepped down in catchup mode because it saw a higher term, check that only
    // the 'numCatchUpsFailedWithNewTerm' primary catchup conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());

    // Check that the targetCatchupOpTime metric was cleared when the node stepped down.
    ASSERT_EQUALS(boost::none,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());
}

TEST_F(PrimaryCatchUpTest, PrimaryStepsDownDuringDrainMode) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);

    processHeartbeatRequests([this, time2](const NetworkOpIter noi) {
        auto net = getNet();
        // The old primary accepted one more op and all nodes caught up after voting for me.
        net->scheduleResponse(noi, net->now(), makeHeartbeatResponse(time2));
    });
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    advanceMyLastWrittenAndAppliedOpTime(time2, Date_t() + Seconds(time2.getSecs()));
    ASSERT(replCoord->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Caught up to the latest"));

    // Check that the number of elections requiring primary catchup was incremented.
    auto opCtx = makeOperationContext();
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsSucceeded' primary catchup conclusion reason was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());

    // Step down during drain mode.
    TopologyCoordinator::UpdateTermResult updateTermResult;
    auto evh = replCoord->updateTerm_forTest(2, &updateTermResult);
    ASSERT_TRUE(evh.isValid());
    getReplExec()->waitForEvent(evh);
    ASSERT_TRUE(replCoord->getMemberState().secondary());

    // Step up again
    ASSERT(replCoord->getApplierState() == ApplierState::Running);
    simulateSuccessfulV1Voting();
    ASSERT_TRUE(replCoord->getMemberState().primary());

    // No need to catch up, so we enter drain mode.
    processHeartbeatRequests([this, time2](const NetworkOpIter noi) {
        auto net = getNet();
        net->scheduleResponse(noi, net->now(), makeHeartbeatResponse(time2));
    });
    ASSERT(replCoord->getApplierState() == ApplierState::Draining);
    {
        Lock::GlobalLock lock(opCtx.get(), MODE_IX);
        ASSERT_FALSE(replCoord->canAcceptWritesForDatabase(
            opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));
    }
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT(replCoord->getApplierState() == ApplierState::Stopped);
    ASSERT_TRUE(replCoord->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented again.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsAlreadyCaughtUp' primary catchup conclusion reason was
    // incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, FreshestNodeBecomesAvailableLater) {
    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(200, 1), 0);
    OpTime time3(Timestamp(300, 1), 0);
    OpTime time4(Timestamp(400, 1), 0);

    // 1) The primary is at time 1 at the beginning.
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);

    // 2) It cannot see all nodes. It learns of time 3 from one node, but the other isn't available.
    //    So the target optime is time 3.
    startCapturingLogMessages();
    auto oneThirdOfTimeout = getNet()->now() + config.getCatchUpTimeoutPeriod() / 3;
    replyHeartbeatsAndRunUntil(oneThirdOfTimeout, [this, time3](const NetworkOpIter noi) {
        const RemoteCommandRequest& request = noi->getRequest();
        if (request.target.host() == "node2") {
            auto status = Status(ErrorCodes::HostUnreachable, "Can't reach remote host");
            getNet()->scheduleResponse(noi, getNet()->now(), status);
        } else {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time3));
        }
    });
    // The node is still in catchup mode, but the target optime has been set.
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    stopCapturingLogMessages();
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Heartbeats updated catchup target optime"));
    ASSERT_EQUALS(time3,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());

    // 3) Advancing its applied optime to time 2 isn't enough.
    advanceMyLastWrittenAndAppliedOpTime(time2, Date_t() + Seconds(time2.getSecs()));
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    // 4) After a while, the other node at time 4 becomes available. Time 4 becomes the new target.
    startCapturingLogMessages();
    auto twoThirdsOfTimeout = getNet()->now() + config.getCatchUpTimeoutPeriod() * 2 / 3;
    replyHeartbeatsAndRunUntil(twoThirdsOfTimeout, [this, time3, time4](const NetworkOpIter noi) {
        const RemoteCommandRequest& request = noi->getRequest();
        if (request.target.host() == "node2") {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time4));
        } else {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time3));
        }
    });
    // The node is still in catchup mode, but the target optime has been updated.
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    stopCapturingLogMessages();
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Heartbeats updated catchup target optime"));
    ASSERT_EQUALS(time4,
                  ReplicationMetrics::get(getServiceContext()).getTargetCatchupOpTime_forTesting());

    // 5) Advancing to time 3 isn't enough now.
    advanceMyLastWrittenAndAppliedOpTime(time3, Date_t() + Seconds(time3.getSecs()));
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    // 6) The node catches up time 4 eventually.
    startCapturingLogMessages();
    advanceMyLastWrittenAndAppliedOpTime(time4, Date_t() + Seconds(time4.getSecs()));
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Caught up to the latest"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsSucceeded' primary catchup conclusion reason was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, InfiniteTimeoutAndAbort) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    auto infiniteTimeout = ReplSetConfig::kInfiniteCatchUpTimeout.count();
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1, infiniteTimeout);

    // Run time far forward and ensure we are still in catchup mode.
    // This is an arbitrary time 'far' into the future.
    auto later = getNet()->now() + config.getElectionTimeoutPeriod() * 10;
    replyHeartbeatsAndRunUntil(later, [this, &config, time2](const NetworkOpIter noi) {
        // Other nodes are ahead of me.
        getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time2));

        // Simulate the heartbeats from secondaries to primary to update liveness info.
        // TODO(sz): Remove this after merging liveness info and heartbeats.
        const RemoteCommandRequest& request = noi->getRequest();
        ReplSetHeartbeatArgsV1 hbArgs;
        hbArgs.setConfigVersion(config.getConfigVersion());
        hbArgs.setSetName(config.getReplSetName());
        hbArgs.setSenderHost(request.target);
        hbArgs.setSenderId(config.findMemberByHostAndPort(request.target)->getId().getData());
        hbArgs.setTerm(getReplCoord()->getTerm());
        ASSERT(hbArgs.isInitialized());
        ReplSetHeartbeatResponse response;
        ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &response));
    });
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    // Simulate a user initiated abort.
    ASSERT_OK(getReplCoord()->abortCatchupIfNeeded(
        ReplicationCoordinator::PrimaryCatchUpConclusionReason::
            kFailedWithReplSetAbortPrimaryCatchUpCmd));
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd' primary catchup
    // conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(1,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, ZeroTimeout) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1, 0);
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Skipping primary catchup"));
    auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(
        opCtx.get(), DatabaseName::createDatabaseName_forTest(boost::none, "test")));

    // Check that the number of elections requiring primary catchup was not incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsSkipped' primary catchup conclusion reason was incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

TEST_F(PrimaryCatchUpTest, CatchUpFailsDueToPrimaryStepDown) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    // Step down in the middle of catchup.
    auto abortTime = getNet()->now() + config.getCatchUpTimeoutPeriod() / 2;
    replyHeartbeatsAndRunUntil(abortTime, [this, time2](const NetworkOpIter noi) {
        // Other nodes are ahead of me.
        getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time2));
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    auto opCtx = makeOperationContext();
    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("Catchup timed out"));

    // Check that the number of elections requiring primary catchup was incremented.
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUps_forTesting());

    // Check that only the 'numCatchUpsFailedWithError' primary catchup conclusion reason was
    // incremented.
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSucceeded_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsAlreadyCaughtUp_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsSkipped_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsTimedOut_forTesting());
    ASSERT_EQ(1, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithError_forTesting());
    ASSERT_EQ(0, ReplicationMetrics::get(opCtx.get()).getNumCatchUpsFailedWithNewTerm_forTesting());
    ASSERT_EQ(0,
              ReplicationMetrics::get(opCtx.get())
                  .getNumCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd_forTesting());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
