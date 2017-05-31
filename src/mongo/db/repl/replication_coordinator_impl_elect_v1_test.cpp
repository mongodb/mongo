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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using ApplierState = ReplicationCoordinator::ApplierState;

TEST_F(ReplCoordTest, RandomizedElectionOffsetWithinProperBounds) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
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
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1
                             << "settings"
                             << BSON("electionTimeoutMillis" << 1));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));

    // Make sure that an election timeout of 1ms doesn't make the random number
    // generator attempt to divide by zero.
    Milliseconds randomOffset = getReplCoord()->getRandomizedElectionOffset_forTest();
    ASSERT_EQ(Milliseconds(0), randomOffset);
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenNodeIsTheOnlyElectableNode) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"
                                                        << "votes"
                                                        << 0
                                                        << "hidden"
                                                        << true
                                                        << "priority"
                                                        << 0))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));

    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);

    ASSERT(getReplCoord()->getMemberState().secondary())
        << getReplCoord()->getMemberState().toString();

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 0), 0));

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->now() < electionTimeoutWhen) {
        net->runUntil(electionTimeoutWhen);
        if (!net->hasReadyRequests()) {
            continue;
        }
        auto noi = net->getNextReadyRequest();
        const auto& request = noi->getRequest();
        error() << "Black holing irrelevant request to " << request.target << ": "
                << request.cmdObj;
        net->blackHole(noi);
    }
    net->exitNetwork();

    // _startElectSelfV1 is called when election timeout expires, so election
    // finished event has been set.
    getReplCoord()->waitForElectionFinish_forTest();

    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    simulateCatchUpAbort();
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    const auto txnPtr = makeOperationContext();
    auto& txn = *txnPtr;

    // Since we're still in drain mode, expect that we report ismaster: false, issecondary:true.
    IsMasterResponse imResponse;
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    getReplCoord()->signalDrainComplete(&txn, getReplCoord()->getTerm());
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();
}

TEST_F(ReplCoordTest, StartElectionDoesNotStartAnElectionWhenNodeIsRecovering) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));

    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT(getReplCoord()->getMemberState().recovering())
        << getReplCoord()->getMemberState().toString();

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 0), 0));
    simulateEnoughHeartbeatsForAllNodesUp();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenNodeIsTheOnlyNode) {
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    getReplCoord()->waitForElectionFinish_forTest();
    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    const auto txnPtr = makeOperationContext();
    auto& txn = *txnPtr;

    // Since we're still in drain mode, expect that we report ismaster: false, issecondary:true.
    IsMasterResponse imResponse;
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    getReplCoord()->signalDrainComplete(&txn, getReplCoord()->getTerm());
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenAllNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    OperationContextNoop txn;
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    startCapturingLogMessages();
    simulateSuccessfulV1Election();
    getReplCoord()->waitForElectionFinish_forTest();

    // Check last vote
    auto lastVote = getExternalState()->loadLocalLastVoteDocument(nullptr);
    ASSERT(lastVote.isOK());
    ASSERT_EQ(0, lastVote.getValue().getCandidateIndex());
    ASSERT_EQ(1, lastVote.getValue().getTerm());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
}

TEST_F(ReplCoordTest, ElectionSucceedsWhenMaxSevenNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
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
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    OperationContextNoop txn;
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    startCapturingLogMessages();
    simulateSuccessfulV1Election();
    getReplCoord()->waitForElectionFinish_forTest();

    // Check last vote
    auto lastVote = getExternalState()->loadLocalLastVoteDocument(nullptr);
    ASSERT(lastVote.isOK());
    ASSERT_EQ(0, lastVote.getValue().getCandidateIndex());
    ASSERT_EQ(1, lastVote.getValue().getTerm());

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
}

TEST_F(ReplCoordTest, ElectionFailsWhenInsufficientVotesAreReceivedDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

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
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON(
                                      "ok" << 1 << "term" << 0 << "voteGranted" << false << "reason"
                                           << "don't like him much")));
            voteRequests++;
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countLogLinesContaining("not running for primary, we received insufficient votes"));
}

TEST_F(ReplCoordTest, ElectionFailsWhenDryRunResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

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
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long() + 1
                                             << "voteGranted"
                                             << false
                                             << "reason"
                                             << "quit living in the past")));
            voteRequests++;
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countLogLinesContaining("not running for primary, we have been superceded already"));
}

TEST_F(ReplCoordTest, NodeWillNotStandForElectionDuringHeartbeatReconfig) {
    // start up, receive reconfig via heartbeat while at the same time, become candidate.
    // candidate state should be cleared.
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
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
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 0), 0));

    // set hbreconfig to hang while in progress
    getExternalState()->setStoreLocalConfigDocumentToHang(true);

    // hb reconfig
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    ReplSetHeartbeatResponse hbResp2;
    ReplSetConfig config;
    config.initialize(BSON("_id"
                           << "mySet"
                           << "version"
                           << 3
                           << "members"
                           << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                    << "node1:12345")
                                         << BSON("_id" << 2 << "host"
                                                       << "node2:12345"))
                           << "protocolVersion"
                           << 1));
    hbResp2.setConfig(config);
    hbResp2.setConfigVersion(3);
    hbResp2.setSetName("mySet");
    hbResp2.setState(MemberState::RS_SECONDARY);
    net->runUntil(net->now() + Seconds(10));  // run until we've sent a heartbeat request
    const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
    net->scheduleResponse(noi2, net->now(), makeResponseStatus(hbResp2.toBSON(true)));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();

    // prepare candidacy
    BSONObjBuilder result;
    ReplicationCoordinator::ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = config.toBSON();
    ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                  getReplCoord()->processReplSetReconfig(&txn, args, &result));

    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(2));
    startCapturingLogMessages();

    // receive sufficient heartbeats to allow the node to see a majority.
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    net->enterNetwork();
    for (int i = 0; i < 2; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            BSONObjBuilder respObj;
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true)));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    // Advance the simulator clock sufficiently to trigger an election.
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

    net->enterNetwork();
    while (net->now() < electionTimeoutWhen) {
        net->runUntil(electionTimeoutWhen);
        if (!net->hasReadyRequests()) {
            continue;
        }
        net->blackHole(net->getNextReadyRequest());
    }
    net->exitNetwork();

    stopCapturingLogMessages();
    // ensure node does not stand for election
    ASSERT_EQUALS(1,
                  countLogLinesContaining("Not standing for election; processing "
                                          "a configuration change"));
    getExternalState()->setStoreLocalConfigDocumentToHang(false);
}

TEST_F(ReplCoordTest, ElectionFailsWhenInsufficientVotesAreReceivedDuringRequestVotes) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON(
                                      "ok" << 1 << "term" << 1 << "voteGranted" << false << "reason"
                                           << "don't like him much")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not becoming primary, we received insufficient votes"));
}

TEST_F(ReplCoordTest, ElectionsAbortWhenNodeTransitionsToRollbackState) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    bool success = false;
    auto event = getReplCoord()->setFollowerMode_nonBlocking(MemberState::RS_ROLLBACK, &success);

    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the vote requester.
    getReplCoord()->waitForElectionFinish_forTest();
    getReplExec()->waitForEvent(event);
    ASSERT_TRUE(success);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
}

TEST_F(ReplCoordTest, ElectionFailsWhenVoteRequestResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long() + 1
                                             << "voteGranted"
                                             << false
                                             << "reason"
                                             << "quit living in the past")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not becoming primary, we have been superceded already"));
}

TEST_F(ReplCoordTest, ElectionFailsWhenTermChangesDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);

    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();

    auto onDryRunRequest = [this](const RemoteCommandRequest& request) {
        // Update to a future term before dry run completes.
        ASSERT_EQUALS(0, request.cmdObj.getIntField("candidateIndex"));
        ASSERT(getTopoCoord().updateTerm(1000, getNet()->now()) ==
               TopologyCoordinator::UpdateTermResult::kUpdatedTerm);
    };
    simulateSuccessfulDryRun(onDryRunRequest);

    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countLogLinesContaining("not running for primary, we have been superceded already"));
}

TEST_F(ReplCoordTest, ElectionFailsWhenTermChangesDuringActualElection) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();
    // update to a future term before the election completes
    getReplCoord()->updateTerm(&txn, 1000);

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON(
                    "ok" << 1 << "term" << request.cmdObj["term"].Long() << "voteGranted" << true
                         << "reason"
                         << "")));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    getReplCoord()->waitForElectionFinish_forTest();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not becoming primary, we have been superceded already"));
}

class PriorityTakeoverTest : public ReplCoordTest {
public:
    /*
     * Verify that a given priority takeover delay is valid. Takeover delays are
     * verified in terms of bounds since the delay value is randomized.
     */
    void assertValidTakeoverDelay(ReplSetConfig config,
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

    void performSuccessfulPriorityTakeover(Date_t priorityTakeoverTime) {
        startCapturingLogMessages();
        simulateSuccessfulV1ElectionAt(priorityTakeoverTime);
        getReplCoord()->waitForElectionFinish_forTest();
        stopCapturingLogMessages();

        ASSERT(getReplCoord()->getMemberState().primary());

        // Check last vote
        auto lastVote = getExternalState()->loadLocalLastVoteDocument(nullptr);
        ASSERT(lastVote.isOK());
        ASSERT_EQ(0, lastVote.getValue().getCandidateIndex());
        ASSERT_EQ(1, lastVote.getValue().getTerm());

        ASSERT_EQUALS(1, countLogLinesContaining("Starting an election for a priority takeover"));
        ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
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
            auto noi = net->getNextReadyRequest();
            auto&& request = noi->getRequest();

            log() << request.target << " processing " << request.cmdObj;
            ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

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
            hbResp.setAppliedOpTime(otherNodesOpTime);
            hbResp.setDurableOpTime(otherNodesOpTime);
            auto response = makeResponseStatus(hbResp.toBSON(replCoord->isV1ElectionProtocol()));
            net->scheduleResponse(noi, net->now(), response);
        }
    }
};

TEST_F(PriorityTakeoverTest, SchedulesPriorityTakeoverIfNodeHasHigherPriorityThanCurrentPrimary) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority"
                                                      << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OperationContextNoop txn;
    OpTime myOptime(Timestamp(100, 1), 0);
    replCoord->setMyLastAppliedOpTime(myOptime);
    replCoord->setMyLastDurableOpTime(myOptime);

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we supersede priorities of all other nodes, prompting the scheduling of a priority
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), myOptime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Also make sure that updating the term cancels the scheduled priority takeover.
    ASSERT_EQUALS(ErrorCodes::StaleTerm, replCoord->updateTerm(&txn, replCoord->getTerm() + 1));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());
}

TEST_F(PriorityTakeoverTest, SuccessfulPriorityTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority"
                                                      << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();
    auto now = getNet()->now();

    OperationContextNoop txn;
    OpTime myOptime(Timestamp(100, 1), 0);
    replCoord->setMyLastAppliedOpTime(myOptime);
    replCoord->setMyLastDurableOpTime(myOptime);

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses, which should give us enough information to know
    // that we supersede priorities of all other nodes, prompting the scheduling of a priority
    // takeover.
    now = respondToHeartbeatsUntil(config, now, HostAndPort("node2", 12345), myOptime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // The priority takeover might be scheduled at a time later than one election
    // timeout after our initial heartbeat responses, so mock another round of
    // heartbeat responses to prevent a normal election timeout.
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, now + halfElectionTimeout, HostAndPort("node2", 12345), myOptime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

TEST_F(PriorityTakeoverTest, DontCallForPriorityTakeoverWhenLaggedSameSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority"
                                                      << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto timeZero = getNet()->now();
    auto now = getNet()->now();

    OperationContextNoop txn;
    OpTime currentOpTime(Timestamp(100, 5000), 0);
    OpTime behindOpTime(Timestamp(100, 3999), 0);
    OpTime closeEnoughOpTime(Timestamp(100, 4000), 0);

    replCoord->setMyLastAppliedOpTime(behindOpTime);
    replCoord->setMyLastDurableOpTime(behindOpTime);

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());

    // Mock a first round of heartbeat responses.
    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, currentOpTime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    now = respondToHeartbeatsUntil(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();

    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(1,
                  countLogLinesContaining("Not standing for election because member is not "
                                          "caught up enough to the most up-to-date member to "
                                          "call for priority takeover"));

    // Mock another round of heartbeat responses that occur after the previous
    // 'priorityTakeoverTime', which should schedule a new priority takeover
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, timeZero + halfElectionTimeout * 3, primaryHostAndPort, currentOpTime);

    // Make sure that a new priority takeover has been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoord->setMyLastAppliedOpTime(closeEnoughOpTime);
    replCoord->setMyLastDurableOpTime(closeEnoughOpTime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

TEST_F(PriorityTakeoverTest, DontCallForPriorityTakeoverWhenLaggedDifferentSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority"
                                                      << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345"))
                             << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();
    auto timeZero = getNet()->now();
    auto now = getNet()->now();

    OperationContextNoop txn;
    OpTime currentOpTime(Timestamp(100, 0), 0);
    OpTime behindOpTime(Timestamp(97, 0), 0);
    OpTime closeEnoughOpTime(Timestamp(98, 0), 0);
    replCoord->setMyLastAppliedOpTime(behindOpTime);
    replCoord->setMyLastDurableOpTime(behindOpTime);

    // Make sure we're secondary and that no priority takeover has been scheduled.
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_FALSE(replCoord->getPriorityTakeover_forTest());


    now = respondToHeartbeatsUntil(config, now, primaryHostAndPort, currentOpTime);

    // Make sure that the priority takeover has actually been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    now = respondToHeartbeatsUntil(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();

    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(1,
                  countLogLinesContaining("Not standing for election because member is not "
                                          "caught up enough to the most up-to-date member to "
                                          "call for priority takeover"));

    // Mock another round of heartbeat responses that occur after the previous
    // 'priorityTakeoverTime', which should schedule a new priority takeover
    Milliseconds halfElectionTimeout = config.getElectionTimeoutPeriod() / 2;
    now = respondToHeartbeatsUntil(
        config, timeZero + halfElectionTimeout * 3, primaryHostAndPort, currentOpTime);

    // Make sure that a new priority takeover has been scheduled and at the
    // correct time.
    ASSERT(replCoord->getPriorityTakeover_forTest());
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest().get();
    assertValidTakeoverDelay(config, now, priorityTakeoverTime, 0);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoord->setMyLastAppliedOpTime(closeEnoughOpTime);
    replCoord->setMyLastDurableOpTime(closeEnoughOpTime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

TEST_F(ReplCoordTest, NodeCancelsElectionUponReceivingANewConfigDuringDryRun) {
    // Start up and become electable.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings"
                            << BSON("heartbeatIntervalMillis" << 100)),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 0), 0));
    simulateEnoughHeartbeatsForAllNodesUp();

    // Advance to dry run vote request phase.
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (TopologyCoordinator::Role::candidate != getTopoCoord().getRole()) {
        net->runUntil(net->now() + Seconds(1));
        if (!net->hasReadyRequests()) {
            continue;
        }
        net->blackHole(net->getNextReadyRequest());
    }
    net->exitNetwork();
    ASSERT(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // Submit a reconfig and confirm it cancels the election.
    ReplicationCoordinatorImpl::ReplSetReconfigArgs config = {
        BSON("_id"
             << "mySet"
             << "version"
             << 4
             << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345"))),
        true};

    BSONObjBuilder result;
    const auto txn = makeOperationContext();
    ASSERT_OK(getReplCoord()->processReplSetReconfig(txn.get(), config, &result));
    // Wait until election cancels.
    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();
    ASSERT(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(ReplCoordTest, NodeCancelsElectionUponReceivingANewConfigDuringVotePhase) {
    // Start up and become electable.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))
                            << "settings"
                            << BSON("heartbeatIntervalMillis" << 100)),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 0), 0));
    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();
    ASSERT(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // Submit a reconfig and confirm it cancels the election.
    ReplicationCoordinatorImpl::ReplSetReconfigArgs config = {
        BSON("_id"
             << "mySet"
             << "version"
             << 4
             << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345"))),
        true};

    BSONObjBuilder result;
    const auto txn = makeOperationContext();
    ASSERT_OK(getReplCoord()->processReplSetReconfig(txn.get(), config, &result));
    // Wait until election cancels.
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    ASSERT(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

class PrimaryCatchUpTest : public ReplCoordTest {
protected:
    using NetworkOpIter = NetworkInterfaceMock::NetworkOperationIterator;
    using NetworkRequestFn = stdx::function<void(const NetworkOpIter)>;

    const Timestamp smallTimestamp{1, 1};

    ResponseStatus makeHeartbeatResponse(OpTime opTime) {
        ReplSetConfig rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(rsConfig.getReplSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        hbResp.setAppliedOpTime(opTime);
        hbResp.setDurableOpTime(opTime);
        return makeResponseStatus(hbResp.toBSON(true));
    }

    void simulateSuccessfulV1Voting() {
        ReplicationCoordinatorImpl* replCoord = getReplCoord();
        NetworkInterfaceMock* net = getNet();

        auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
        ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
        log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

        ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
        // Process requests until we're primary but leave the heartbeats for the notification
        // of election win. Exit immediately on unexpected requests.
        while (!replCoord->getMemberState().primary()) {
            log() << "Waiting on network in state " << replCoord->getMemberState();
            net->enterNetwork();
            if (net->now() < electionTimeoutWhen) {
                net->runUntil(electionTimeoutWhen);
            }
            // Peek the next request, don't consume it yet.
            const NetworkOpIter noi = net->getFrontOfUnscheduledQueue();
            const RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            if (ReplSetHeartbeatArgsV1().initialize(request.cmdObj).isOK()) {
                OpTime opTime(Timestamp(), getReplCoord()->getTerm());
                net->scheduleResponse(
                    net->getNextReadyRequest(), net->now(), makeHeartbeatResponse(opTime));
            } else if (request.cmdObj.firstElement().fieldNameStringData() ==
                       "replSetRequestVotes") {
                net->scheduleResponse(net->getNextReadyRequest(),
                                      net->now(),
                                      makeResponseStatus(BSON("ok" << 1 << "reason"
                                                                   << ""
                                                                   << "term"
                                                                   << request.cmdObj["term"].Long()
                                                                   << "voteGranted"
                                                                   << true)));
            } else {
                // Stop the loop and let the caller handle unexpected requests.
                net->exitNetwork();
                break;
            }
            net->runReadyNetworkOperations();
            // Successful elections need to write the last vote to disk, which is done by DB worker.
            // Wait until DB worker finishes its job to ensure the synchronization with the
            // executor.
            getReplExec()->waitForDBWork_forTest();
            net->runReadyNetworkOperations();
            net->exitNetwork();
        }
    }

    ReplSetConfig setUp3NodeReplSetAndRunForElection(OpTime opTime, bool infiniteTimeout = false) {
        BSONObj configObj = BSON("_id"
                                 << "mySet"
                                 << "version"
                                 << 1
                                 << "members"
                                 << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                          << "node1:12345")
                                               << BSON("_id" << 2 << "host"
                                                             << "node2:12345")
                                               << BSON("_id" << 3 << "host"
                                                             << "node3:12345"))
                                 << "protocolVersion"
                                 << 1
                                 << "settings"
                                 << BSON("heartbeatTimeoutSecs" << 1 << "catchUpTimeoutMillis"
                                                                << (infiniteTimeout ? -1 : 5000)));
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplSetConfig config = assertMakeRSConfig(configObj);

        getReplCoord()->setMyLastAppliedOpTime(opTime);
        getReplCoord()->setMyLastDurableOpTime(opTime);
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        simulateSuccessfulV1Voting();
        IsMasterResponse imResponse;
        getReplCoord()->fillIsMasterForReplSet(&imResponse);
        ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
        ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();

        return config;
    }

    ResponseStatus makeFreshnessScanResponse(OpTime opTime) {
        // OpTime part of replSetGetStatus.
        return makeResponseStatus(BSON("optimes" << BSON("appliedOpTime" << opTime.toBSON())));
    }

    void processHeartbeatRequests(NetworkRequestFn onHeartbeatRequest) {
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        while (net->hasReadyRequests()) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            if (ReplSetHeartbeatArgsV1().initialize(request.cmdObj).isOK()) {
                onHeartbeatRequest(noi);
            } else {
                log() << "Black holing unexpected request to " << request.target << ": "
                      << request.cmdObj;
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
                log() << request.target << " at " << net->now() << " processing " << request.cmdObj;
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
    void advanceMyLastAppliedOpTime(OpTime opTime) {
        getReplCoord()->setMyLastAppliedOpTime(opTime);
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
    ASSERT_EQ(1, countLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
}

// Heartbeats set a future target OpTime and we reached that successfully.
TEST_F(PrimaryCatchUpTest, CatchupSucceeds) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1);
    processHeartbeatRequests([this, time2](const NetworkOpIter noi) {
        auto net = getNet();
        // The old primary accepted one more op and all nodes caught up after voting for me.
        net->scheduleResponse(noi, net->now(), makeHeartbeatResponse(time2));
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    advanceMyLastAppliedOpTime(time2);
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("Caught up to the latest known optime successfully"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
    ASSERT_EQ(1, countLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
            log() << "Black holing heartbeat from " << request.target.host();
            getNet()->blackHole(noi);
        } else {
            getNet()->scheduleResponse(noi, getNet()->now(), makeHeartbeatResponse(time1));
        }
    });
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(1, countLogLinesContaining("Caught up to the latest optime known via heartbeats"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
    ASSERT_EQUALS(1, countLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countLogLinesContaining("Catchup timed out"));
    ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    TopologyCoordinator::UpdateTermResult updateTermResult;
    auto evh = getReplCoord()->updateTerm_forTest(2, &updateTermResult);
    ASSERT_TRUE(evh.isValid());
    getReplExec()->waitForEvent(evh);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    //    replyHeartbeatsAndRunUntil(getNet()->now() + config.getCatchUpTimeoutPeriod());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countLogLinesContaining("Catchup timed out"));
    ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase("test"));
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
    advanceMyLastAppliedOpTime(time2);
    ASSERT(replCoord->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("Caught up to the latest"));

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
    ASSERT_FALSE(replCoord->canAcceptWritesForDatabase("test"));
    auto txn = makeOperationContext();
    replCoord->signalDrainComplete(txn.get(), replCoord->getTerm());
    ASSERT(replCoord->getApplierState() == ApplierState::Stopped);
    ASSERT_TRUE(replCoord->canAcceptWritesForDatabase("test"));
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
    ASSERT_EQ(1, countLogLinesContaining("Heartbeats updated catchup target optime"));

    // 3) Advancing its applied optime to time 2 isn't enough.
    advanceMyLastAppliedOpTime(time2);
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
    ASSERT_EQ(1, countLogLinesContaining("Heartbeats updated catchup target optime"));

    // 5) Advancing to time 3 isn't enough now.
    advanceMyLastAppliedOpTime(time3);
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    // 6) The node catches up time 4 eventually.
    startCapturingLogMessages();
    advanceMyLastAppliedOpTime(time4);
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);
    stopCapturingLogMessages();
    ASSERT_EQ(1, countLogLinesContaining("Caught up to the latest"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
}

TEST_F(PrimaryCatchUpTest, InfiniteTimeoutAndAbort) {
    startCapturingLogMessages();

    OpTime time1(Timestamp(100, 1), 0);
    OpTime time2(Timestamp(100, 2), 0);
    ReplSetConfig config = setUp3NodeReplSetAndRunForElection(time1, true);

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
        hbArgs.setSenderId(config.findMemberByHostAndPort(request.target)->getId());
        hbArgs.setTerm(getReplCoord()->getTerm());
        ASSERT(hbArgs.isInitialized());
        ReplSetHeartbeatResponse response;
        ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &response));
    });
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Running);

    // Simulate a user initiated abort.
    ASSERT_OK(getReplCoord()->abortCatchupIfNeeded());
    ASSERT(getReplCoord()->getApplierState() == ApplierState::Draining);

    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("Exited primary catch-up mode"));
    ASSERT_EQUALS(0, countLogLinesContaining("Caught up to the latest"));
    ASSERT_EQUALS(0, countLogLinesContaining("Catchup timed out"));
    auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase("test"));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
