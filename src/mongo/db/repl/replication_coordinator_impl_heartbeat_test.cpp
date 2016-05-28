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

class ReplCoordHBTest : public ReplCoordTest {
protected:
    void assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost);
    void assertMemberState(MemberState expected, std::string msg = "");
    ReplSetHeartbeatResponse receiveHeartbeatFrom(const ReplicaSetConfig& rsConfig,
                                                  int sourceId,
                                                  const HostAndPort& source);
};

void ReplCoordHBTest::assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost) {
    ReplCoordTest::assertStartSuccess(addProtocolVersion(configDoc, 0), selfHost);
}

void ReplCoordHBTest::assertMemberState(const MemberState expected, std::string msg) {
    const MemberState actual = getReplCoord()->getMemberState();
    ASSERT(expected == actual) << "Expected coordinator to report state " << expected.toString()
                               << " but found " << actual.toString() << " - " << msg;
}

ReplSetHeartbeatResponse ReplCoordHBTest::receiveHeartbeatFrom(const ReplicaSetConfig& rsConfig,
                                                               int sourceId,
                                                               const HostAndPort& source) {
    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(rsConfig.getConfigVersion());
    hbArgs.setSetName(rsConfig.getReplSetName());
    hbArgs.setSenderHost(source);
    hbArgs.setSenderId(sourceId);
    ASSERT(hbArgs.isInitialized());

    ReplSetHeartbeatResponse response;
    ASSERT_OK(getReplCoord()->processHeartbeat(hbArgs, &response));
    return response;
}

TEST_F(ReplCoordHBTest, NodeJoinsExistingReplSetWhenReceivingAConfigContainingTheNodeViaHeartbeat) {
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
    ReplicaSetConfig rsConfig = assertMakeRSConfigV0(BSON("_id"
                                                          << "mySet"
                                                          << "version"
                                                          << 3
                                                          << "members"
                                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                   << "h1:1")
                                                                        << BSON("_id" << 2 << "host"
                                                                                      << "h2:1")
                                                                        << BSON("_id" << 3 << "host"
                                                                                      << "h3:1"))));
    init("mySet");
    addSelf(HostAndPort("h2", 1));
    const Date_t startDate = getNet()->now();
    start();
    enterNetwork();
    assertMemberState(MemberState::RS_STARTUP);
    NetworkInterfaceMock* net = getNet();
    ASSERT_FALSE(net->hasReadyRequests());
    exitNetwork();
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), request.target);
    ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.noteReplSet();
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder, false);
    net->scheduleResponse(
        noi, startDate + Milliseconds(200), makeResponseStatus(responseBuilder.obj()));
    assertRunUntil(startDate + Milliseconds(200));

    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    noi = net->getNextReadyRequest();

    assertMemberState(MemberState::RS_STARTUP2);
    OperationContextNoop txn;
    ReplicaSetConfig storedConfig;
    ASSERT_OK(storedConfig.initialize(
        unittest::assertGet(getExternalState()->loadLocalConfigDocument(&txn))));
    ASSERT_OK(storedConfig.validate());
    ASSERT_EQUALS(3, storedConfig.getConfigVersion());
    ASSERT_EQUALS(3, storedConfig.getNumMembers());
    exitNetwork();
}

TEST_F(ReplCoordHBTest,
       NodeDoesNotJoinExistingReplSetWhenReceivingAConfigNotContainingTheNodeViaHeartbeat) {
    // Tests that a node in RS_STARTUP will not transition to RS_REMOVED if it receives a
    // configuration that does not contain it.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
    ReplicaSetConfig rsConfig = assertMakeRSConfigV0(BSON("_id"
                                                          << "mySet"
                                                          << "version"
                                                          << 3
                                                          << "members"
                                                          << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                                   << "h1:1")
                                                                        << BSON("_id" << 2 << "host"
                                                                                      << "h2:1")
                                                                        << BSON("_id" << 3 << "host"
                                                                                      << "h3:1"))));
    init("mySet");
    addSelf(HostAndPort("h4", 1));
    const Date_t startDate = getNet()->now();
    start();
    enterNetwork();
    assertMemberState(MemberState::RS_STARTUP, "1");
    NetworkInterfaceMock* net = getNet();
    ASSERT_FALSE(net->hasReadyRequests());
    exitNetwork();
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), request.target);
    ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.noteReplSet();
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder, false);
    net->scheduleResponse(
        noi, startDate + Milliseconds(200), makeResponseStatus(responseBuilder.obj()));
    assertRunUntil(startDate + Milliseconds(2200));

    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    noi = net->getNextReadyRequest();

    assertMemberState(MemberState::RS_STARTUP, "2");
    OperationContextNoop txn;

    StatusWith<BSONObj> loadedConfig(getExternalState()->loadLocalConfigDocument(&txn));
    ASSERT_NOT_OK(loadedConfig.getStatus()) << loadedConfig.getValue();
    exitNetwork();
}

TEST_F(ReplCoordHBTest, NodeReturnsNotYetInitializedInResponseToAHeartbeatReceivedPriorToAConfig) {
    // ensure that if we've yet to receive an initial config, we return NotYetInitialized
    init("mySet");
    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(3);
    hbArgs.setSetName("mySet");
    hbArgs.setSenderHost(HostAndPort("h1:1"));
    hbArgs.setSenderId(1);
    ASSERT(hbArgs.isInitialized());

    ReplSetHeartbeatResponse response;
    Status status = getReplCoord()->processHeartbeat(hbArgs, &response);
    ASSERT_EQUALS(ErrorCodes::NotYetInitialized, status.code());
}

TEST_F(ReplCoordHBTest,
       NodeChangesToRecoveringStateWhenAllNodesRespondToHeartbeatsWithUnauthorized) {
    // Tests that a node that only has auth error heartbeats is recovering
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
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

    // process heartbeat
    enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    log() << request.target.toString() << " processing " << request.cmdObj;
    getNet()->scheduleResponse(noi,
                               getNet()->now(),
                               makeResponseStatus(BSON("ok" << 0.0 << "errmsg"
                                                            << "unauth'd"
                                                            << "code"
                                                            << ErrorCodes::Unauthorized)));

    if (request.target != HostAndPort("node2", 12345) &&
        request.cmdObj.firstElement().fieldNameStringData() != "replSetHeartbeat") {
        error() << "Black holing unexpected request to " << request.target << ": "
                << request.cmdObj;
        getNet()->blackHole(noi);
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    ASSERT_TRUE(getTopoCoord().getMemberState().recovering());
    assertMemberState(MemberState::RS_RECOVERING, "0");
}

}  // namespace
}  // namespace repl
}  // namespace mongo
