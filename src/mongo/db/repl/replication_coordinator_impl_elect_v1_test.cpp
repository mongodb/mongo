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
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config.h"
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

class ReplCoordElectV1Test : public ReplCoordTest {
protected:
    void simulateEnoughHeartbeatsForElectability();
};

void ReplCoordElectV1Test::simulateEnoughHeartbeatsForElectability() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
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
}

TEST_F(ReplCoordElectV1Test, ElectionSucceedsWhenNodeIsTheOnlyElectableNode) {
    OperationContextReplMock txn;
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
    ASSERT(getReplCoord()->isWaitingForApplierToDrain());

    // Since we're still in drain mode, expect that we report ismaster: false, issecondary:true.
    IsMasterResponse imResponse;
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    getReplCoord()->signalDrainComplete(&txn);
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();
}

TEST_F(ReplCoordElectV1Test, StartElectionDoesNotStartAnElectionWhenNodeIsRecovering) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345")) << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));

    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_RECOVERING));

    ASSERT(getReplCoord()->getMemberState().recovering())
        << getReplCoord()->getMemberState().toString();

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 0), 0));
    simulateEnoughHeartbeatsForElectability();

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordElectV1Test, ElectionSucceedsWhenNodeIsTheOnlyNode) {
    OperationContextReplMock txn;
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")) << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 0), 0));
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    getReplCoord()->waitForElectionFinish_forTest();
    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    ASSERT(getReplCoord()->isWaitingForApplierToDrain());

    // Since we're still in drain mode, expect that we report ismaster: false, issecondary:true.
    IsMasterResponse imResponse;
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    getReplCoord()->signalDrainComplete(&txn);
    getReplCoord()->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();
}

TEST_F(ReplCoordElectV1Test, ElectionSucceedsWhenAllNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
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

TEST_F(ReplCoordElectV1Test, ElectionSucceedsWhenMaxSevenNodesVoteYea) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(
                                    BSON("_id" << 1 << "host"
                                               << "node1:12345")
                                    << BSON("_id" << 2 << "host"
                                                  << "node2:12345") << BSON("_id" << 3 << "host"
                                                                                  << "node3:12345")
                                    << BSON("_id" << 4 << "host"
                                                  << "node4:12345") << BSON("_id" << 5 << "host"
                                                                                  << "node5:12345")
                                    << BSON("_id" << 6 << "host"
                                                  << "node6:12345") << BSON("_id" << 7 << "host"
                                                                                  << "node7:12345"))
                             << "protocolVersion" << 1);
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

TEST_F(ReplCoordElectV1Test, ElectionFailsWhenInsufficientVotesAreReceivedDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();

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
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 0 << "voteGranted"
                                                               << false << "reason"
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

TEST_F(ReplCoordElectV1Test, ElectionFailsWhenDryRunResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();

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
                                             << "voteGranted" << false << "reason"
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

TEST_F(ReplCoordElectV1Test, NodeWillNotStandForElectionDuringHeartbeatReconfig) {
    // start up, receive reconfig via heartbeat while at the same time, become candidate.
    // candidate state should be cleared.
    OperationContextNoop txn;
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                      << "node1:12345")
                           << BSON("_id" << 2 << "host"
                                         << "node2:12345") << BSON("_id" << 3 << "host"
                                                                         << "node3:12345")
                           << BSON("_id" << 4 << "host"
                                         << "node4:12345") << BSON("_id" << 5 << "host"
                                                                         << "node5:12345"))
             << "protocolVersion" << 1),
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
    ReplicaSetConfig config;
    config.initialize(BSON("_id"
                           << "mySet"
                           << "version" << 3 << "members"
                           << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                    << "node1:12345")
                                         << BSON("_id" << 2 << "host"
                                                       << "node2:12345")) << "protocolVersion"
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
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
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
                  countLogLinesContaining(
                      "Not standing for election; processing "
                      "a configuration change"));
    getExternalState()->setStoreLocalConfigDocumentToHang(false);
}

// This is disabled because DeclaringElectionWinner has been disabled.
// TODO(siyuan) SERVER-19423 Remove election winner declarer
//
// TEST_F(ReplCoordElectV1Test, ElectionSucceedsButDeclaringWinnerFails) {
//    startCapturingLogMessages();
//    BSONObj configObj = BSON("_id"
//                             << "mySet"
//                             << "version" << 1 << "members"
//                             << BSON_ARRAY(BSON("_id" << 1 << "host"
//                                                      << "node1:12345")
//                                           << BSON("_id" << 2 << "host"
//                                                         << "node2:12345")
//                                           << BSON("_id" << 3 << "host"
//                                                         << "node3:12345"))
//                             << "protocolVersion" << 1);
//    assertStartSuccess(configObj, HostAndPort("node1", 12345));
//    ReplicaSetConfig config = assertMakeRSConfig(configObj);
//
//    OperationContextNoop txn;
//    OpTime time1(Timestamp(100, 1), 0);
//    getReplCoord()->setMyLastAppliedOpTime(time1);
//    getReplCoord()->setMyLastDurableOpTime(time1);
//    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
//
//    simulateEnoughHeartbeatsForElectability();
//
//    NetworkInterfaceMock* net = getNet();
//    net->enterNetwork();
//    while (net->hasReadyRequests()) {
//        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
//        const RemoteCommandRequest& request = noi->getRequest();
//        log() << request.target.toString() << " processing " << request.cmdObj;
//        if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
//            net->scheduleResponse(
//                noi,
//                net->now(),
//                makeResponseStatus(BSON("ok" << 1 << "term"
//                                             << (request.cmdObj["dryRun"].Bool()
//                                                     ? request.cmdObj["term"].Long() - 1
//                                                     : request.cmdObj["term"].Long())
//                                             << "voteGranted" << true)));
//        } else if (request.cmdObj.firstElement().fieldNameStringData() ==
//                   "replSetDeclareElectionWinner") {
//            net->scheduleResponse(
//                noi,
//                net->now(),
//                makeResponseStatus(BSON("ok" << 0 << "code" << ErrorCodes::BadValue << "errmsg"
//                                             << "term has already passed"
//                                             << "term" << request.cmdObj["term"].Long() + 1)));
//        } else {
//            error() << "Black holing unexpected request to " << request.target << ": "
//                    << request.cmdObj;
//            net->blackHole(noi);
//        }
//        net->runReadyNetworkOperations();
//    }
//    net->exitNetwork();
//    stopCapturingLogMessages();
//    ASSERT_EQUALS(1, countLogLinesContaining("stepping down from primary, because:"));
//}


TEST_F(ReplCoordElectV1Test, ElectionFailsWhenInsufficientVotesAreReceivedDuringRequestVotes) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();
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
                                  makeResponseStatus(BSON("ok" << 1 << "term" << 1 << "voteGranted"
                                                               << false << "reason"
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

TEST_F(ReplCoordElectV1Test, ElectionsAbortWhenNodeTransitionsToRollbackState) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();
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

TEST_F(ReplCoordElectV1Test, ElectionFailsWhenVoteRequestResponseContainsANewerTerm) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();
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
                                             << "voteGranted" << false << "reason"
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

TEST_F(ReplCoordElectV1Test, ElectionFailsWhenTermChangesDuringDryRun) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);

    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();

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

TEST_F(ReplCoordElectV1Test, ElectionFailsWhenTermChangesDuringActualElection) {
    startCapturingLogMessages();
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForElectability();
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
                makeResponseStatus(BSON("ok" << 1 << "term" << request.cmdObj["term"].Long()
                                             << "voteGranted" << true << "reason"
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
    void respondToAllHeartbeats(const ReplicaSetConfig& config,
                                Date_t runUntil,
                                const HostAndPort& primaryHostAndPort,
                                const OpTime& otherNodesOpTime) {
        auto replCoord = getReplCoord();

        auto net = getNet();
        net->enterNetwork();
        while (net->now() < runUntil || net->hasReadyRequests()) {
            if (net->now() < runUntil) {
                net->runUntil(runUntil);
            }
            auto noi = net->getNextReadyRequest();
            auto&& request = noi->getRequest();
            log() << request.target << " processing " << request.cmdObj;
            ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
            ReplSetHeartbeatArgsV1 hbArgs;
            if (hbArgs.initialize(request.cmdObj).isOK()) {
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
                auto response =
                    makeResponseStatus(hbResp.toBSON(replCoord->isV1ElectionProtocol()));
                net->scheduleResponse(noi, net->now(), response);
            } else {
                error() << "Black holing unexpected request to " << request.target << ": "
                        << request.cmdObj;
                net->blackHole(noi);
            }
        }
        net->runReadyNetworkOperations();
        net->exitNetwork();
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
};

TEST_F(PriorityTakeoverTest, SchedulesPriorityTakeoverIfNodeHasHigherPriorityThanCurrentPrimary) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    replCoord->setMyLastAppliedOpTime(time1);
    replCoord->setMyLastDurableOpTime(time1);
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ASSERT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());

    auto now = getNet()->now();
    respondToAllHeartbeats(config, now, HostAndPort("node2", 12345), time1);

    ASSERT_NOT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0),
                  replCoord->getPriorityTakeover_forTest());

    // Updating term cancels priority takeover callback.
    ASSERT_EQUALS(ErrorCodes::StaleTerm, replCoord->updateTerm(&txn, replCoord->getTerm() + 1));
    ASSERT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());
}

TEST_F(PriorityTakeoverTest, SuccessfulPriorityTakeover) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    replCoord->setMyLastAppliedOpTime(time1);
    replCoord->setMyLastDurableOpTime(time1);
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ASSERT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());

    auto now = getNet()->now();
    respondToAllHeartbeats(config, now, HostAndPort("node2", 12345), time1);

    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest();
    ASSERT_NOT_EQUALS(Date_t(), priorityTakeoverTime);
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0), priorityTakeoverTime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

TEST_F(PriorityTakeoverTest, DontCallForPriorityTakeoverWhenLaggedSameSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();

    OperationContextNoop txn;
    OpTime currentOpTime(Timestamp(100, 5000), 0);
    OpTime behindOpTime(Timestamp(100, 3999), 0);
    OpTime closeEnoughOpTime(Timestamp(100, 4000), 0);
    replCoord->setMyLastAppliedOpTime(behindOpTime);
    replCoord->setMyLastDurableOpTime(behindOpTime);
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ASSERT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());

    auto now = getNet()->now();

    respondToAllHeartbeats(config, now, primaryHostAndPort, currentOpTime);

    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest();
    ASSERT_NOT_EQUALS(Date_t(), priorityTakeoverTime);
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0), priorityTakeoverTime);


    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    respondToAllHeartbeats(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();


    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(1,
                  countLogLinesContaining(
                      "Not standing for election because member is not "
                      "caught up enough to the most up-to-date member to "
                      "call for priority takeover"));

    now = getNet()->now();
    ASSERT_EQUALS(now, priorityTakeoverTime);
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest();
    ASSERT_NOT_EQUALS(Date_t(), priorityTakeoverTime);
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0), priorityTakeoverTime);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoord->setMyLastAppliedOpTime(closeEnoughOpTime);
    replCoord->setMyLastDurableOpTime(closeEnoughOpTime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

TEST_F(PriorityTakeoverTest, DontCallForPriorityTakeoverWhenLaggedDifferentSecond) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 2)
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 3 << "host"
                                                         << "node3:12345")) << "protocolVersion"
                             << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);
    HostAndPort primaryHostAndPort("node2", 12345);

    auto replCoord = getReplCoord();

    OperationContextNoop txn;
    OpTime currentOpTime(Timestamp(100, 0), 0);
    OpTime behindOpTime(Timestamp(97, 0), 0);
    OpTime closeEnoughOpTime(Timestamp(98, 0), 0);
    replCoord->setMyLastAppliedOpTime(behindOpTime);
    replCoord->setMyLastDurableOpTime(behindOpTime);
    ASSERT(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ASSERT_EQUALS(Date_t(), replCoord->getPriorityTakeover_forTest());

    auto now = getNet()->now();

    respondToAllHeartbeats(config, now, primaryHostAndPort, currentOpTime);

    auto priorityTakeoverTime = replCoord->getPriorityTakeover_forTest();
    ASSERT_NOT_EQUALS(Date_t(), priorityTakeoverTime);
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0), priorityTakeoverTime);


    // At this point the other nodes are all ahead of the current node, so it can't call for
    // priority takeover.
    startCapturingLogMessages();
    respondToAllHeartbeats(config, priorityTakeoverTime, primaryHostAndPort, currentOpTime);
    stopCapturingLogMessages();


    ASSERT(replCoord->getMemberState().secondary());
    ASSERT_EQUALS(1,
                  countLogLinesContaining(
                      "Not standing for election because member is not "
                      "caught up enough to the most up-to-date member to "
                      "call for priority takeover"));

    now = getNet()->now();
    ASSERT_EQUALS(now, priorityTakeoverTime);
    priorityTakeoverTime = replCoord->getPriorityTakeover_forTest();
    ASSERT_NOT_EQUALS(Date_t(), priorityTakeoverTime);
    ASSERT_EQUALS(now + config.getPriorityTakeoverDelay(0), priorityTakeoverTime);

    // Now make us caught up enough to call for priority takeover to succeed.
    replCoord->setMyLastAppliedOpTime(closeEnoughOpTime);
    replCoord->setMyLastDurableOpTime(closeEnoughOpTime);

    performSuccessfulPriorityTakeover(priorityTakeoverTime);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
