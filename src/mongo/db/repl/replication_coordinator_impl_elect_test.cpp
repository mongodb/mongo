/**
 *    Copyright (C) 2014 MongoDB Inc.
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
#include "mongo/db/repl/repl_set_heartbeat_args.h"
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

class ReplCoordElectTest : public ReplCoordTest {
protected:
    void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost);
    void simulateFreshEnoughForElectability();
};

void ReplCoordElectTest::assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost) {
    ReplCoordTest::assertStartSuccess(addProtocolVersion(configDoc, 0), selfHost);
}

void ReplCoordElectTest::simulateFreshEnoughForElectability() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.cmdObj.firstElement().fieldNameStringData() == "replSetFresh") {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "fresher" << false << "opTime"
                                             << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL())
                                             << "veto"
                                             << false)));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
}

TEST_F(ReplCoordElectTest, StartElectionDoesNotStartAnElectionWhenNodeHasNoOplogEntries) {
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
    // Election never starts because we haven't set a lastOpTimeApplied value yet, via a
    // heartbeat.
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    simulateEnoughHeartbeatsForAllNodesUp();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("node has no applied oplog entries"));
}

/**
 * This test checks that an election can happen when only one node is up, and it has the
 * vote(s) to win.
 */
TEST_F(ReplCoordElectTest, ElectionSucceedsWhenNodeIsTheOnlyElectableNode) {
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
                                                        << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    // Fake OpTime from initiate, or a write op.
    getExternalState()->setLastOpTime(OpTime{{0, 0}, 0});

    ASSERT(getReplCoord()->getMemberState().secondary())
        << getReplCoord()->getMemberState().toString();

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 0), 0));

    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    // blackhole heartbeat
    net->scheduleResponse(noi, net->now(), ResponseStatus(ErrorCodes::OperationFailed, "timeout"));
    net->runReadyNetworkOperations();
    // blackhole freshness
    const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
    net->scheduleResponse(noi2, net->now(), ResponseStatus(ErrorCodes::OperationFailed, "timeout"));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    ASSERT(getReplCoord()->isWaitingForApplierToDrain());

    const auto txnPtr = makeOperationContext();
    auto& txn = *txnPtr;

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

TEST_F(ReplCoordElectTest, ElectionSucceedsWhenNodeIsTheOnlyNode) {
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);

    // Fake OpTime from initiate, or a write op.
    getExternalState()->setLastOpTime(OpTime{{0, 0}, 0});

    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();
    ASSERT(getReplCoord()->isWaitingForApplierToDrain());

    const auto txnPtr = makeOperationContext();
    auto& txn = *txnPtr;

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

TEST_F(ReplCoordElectTest, ElectionSucceedsWhenAllNodesVoteYea) {
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
    OperationContextNoop txn;
    getReplCoord()->setMyLastAppliedOpTime(OpTime{{100, 1}, 0});
    getExternalState()->setLastOpTime(OpTime{{100, 1}, 0});

    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    startCapturingLogMessages();
    simulateSuccessfulElection();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
}

TEST_F(ReplCoordElectTest, ElectionFailsWhenOneNodeVotesNay) {
    // one responds with -10000 votes, and one doesn't respond, and we are not elected
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
                                                         << "node3:12345")));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateFreshEnoughForElectability();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.target != HostAndPort("node2", 12345)) {
            net->blackHole(noi);
        } else if (request.cmdObj.firstElement().fieldNameStringData() != "replSetElect") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "vote" << -10000 << "round" << OID())));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("couldn't elect self, only received -9999 votes"));
}

TEST_F(ReplCoordElectTest, VotesWithStringValuesAreNotCountedAsYeas) {
    // one responds with a bad 'vote' field, and one doesn't respond, and we are not elected
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
                                                         << "node3:12345")));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateFreshEnoughForElectability();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        if (request.target != HostAndPort("node2", 12345)) {
            net->blackHole(noi);
        } else if (request.cmdObj.firstElement().fieldNameStringData() != "replSetElect") {
            net->blackHole(noi);
        } else {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "vote"
                                                               << "yea"
                                                               << "round"
                                                               << OID())));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("wrong type for vote argument in replSetElect command"));
}

TEST_F(ReplCoordElectTest, ElectionsAbortWhenNodeTransitionsToRollbackState) {
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
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateFreshEnoughForElectability();

    bool success = false;
    auto event = getReplCoord()->setFollowerMode_nonBlocking(MemberState::RS_ROLLBACK, &success);

    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the freshness checker and election command runner.
    getReplCoord()->waitForElectionFinish_forTest();
    getReplExec()->waitForEvent(event);
    ASSERT_TRUE(success);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
}

TEST_F(ReplCoordElectTest, NodeWillNotStandForElectionDuringHeartbeatReconfig) {
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
                                                        << "node5:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 0), 0));

    // set hbreconfig to hang while in progress
    getExternalState()->setStoreLocalConfigDocumentToHang(true);

    // hb reconfig
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    ReplSetHeartbeatResponse hbResp2;
    ReplicaSetConfig config;
    config.initialize(BSON("_id"
                           << "mySet"
                           << "version"
                           << 3
                           << "members"
                           << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                    << "node1:12345")
                                         << BSON("_id" << 2 << "host"
                                                       << "node2:12345"))));
    hbResp2.setConfig(config);
    hbResp2.setConfigVersion(3);
    hbResp2.setSetName("mySet");
    hbResp2.setState(MemberState::RS_SECONDARY);
    BSONObjBuilder respObj2;
    respObj2 << "ok" << 1;
    hbResp2.addToBSON(&respObj2, false);
    net->runUntil(net->now() + Seconds(10));  // run until we've sent a heartbeat request
    const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
    net->scheduleResponse(noi2, net->now(), makeResponseStatus(respObj2.obj()));
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

    // receive sufficient heartbeats to trigger an election
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    net->enterNetwork();
    for (int i = 0; i < 2; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgs hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj, false);
            net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }

    stopCapturingLogMessages();
    // ensure node does not stand for election
    ASSERT_EQUALS(1,
                  countLogLinesContaining("Not standing for election; processing "
                                          "a configuration change"));
    getExternalState()->setStoreLocalConfigDocumentToHang(false);
}

TEST_F(ReplCoordElectTest, StepsDownRemoteIfNodeHasHigherPriorityThanCurrentPrimary) {
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
                                                         << "node3:12345")));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplicaSetConfig config = assertMakeRSConfig(configObj);

    auto replCoord = getReplCoord();

    OperationContextNoop txn;
    OpTime time1(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        auto noi = net->getNextReadyRequest();
        auto&& request = noi->getRequest();
        log() << request.target << " processing " << request.cmdObj;
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
        ReplSetHeartbeatArgs hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(config.getReplSetName());
            if (request.target == HostAndPort("node2", 12345)) {
                hbResp.setState(MemberState::RS_PRIMARY);
            } else {
                hbResp.setState(MemberState::RS_SECONDARY);
            }
            hbResp.setConfigVersion(config.getConfigVersion());
            auto response = makeResponseStatus(hbResp.toBSON(replCoord->isV1ElectionProtocol()));
            net->scheduleResponse(noi, net->now(), response);
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
    }
    net->runReadyNetworkOperations();
    net->exitNetwork();

    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    log() << request.target << " processing " << request.cmdObj;
    ASSERT_EQUALS("replSetStepDown", request.cmdObj.firstElement().fieldNameStringData());
    auto target = request.target;
    ASSERT_EQUALS(HostAndPort("node2", 12345), target);
    auto response = makeResponseStatus(BSON("ok" << 1));
    net->scheduleResponse(noi, net->now(), response);
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
    startCapturingLogMessages();
    net->runReadyNetworkOperations();
    stopCapturingLogMessages();
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Log());
    net->exitNetwork();
    ASSERT_EQUALS(1,
                  countLogLinesContaining(str::stream() << "stepdown of primary("
                                                        << target.toString()
                                                        << ") succeeded"));
}

TEST_F(ReplCoordElectTest, NodeCancelsElectionUponReceivingANewConfigDuringFreshnessCheckingPhase) {
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
    simulateFreshEnoughForElectability();
    ASSERT(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // Advance to freshness checker request phase.
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

TEST_F(ReplCoordElectTest, NodeCancelsElectionUponReceivingANewConfigDuringElectionPhase) {
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
    simulateFreshEnoughForElectability();
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

}  // namespace
}  // namespace repl
}  // namespace mongo
