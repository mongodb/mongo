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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/task_runner_test_fixture.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class ReplCoordHBV1Test : public ReplCoordTest {
protected:
    void assertMemberState(MemberState expected, std::string msg = "");
    ReplSetHeartbeatResponse receiveHeartbeatFrom(const ReplSetConfig& rsConfig,
                                                  int sourceId,
                                                  const HostAndPort& source);
};

void ReplCoordHBV1Test::assertMemberState(const MemberState expected, std::string msg) {
    const MemberState actual = getReplCoord()->getMemberState();
    ASSERT(expected == actual) << "Expected coordinator to report state " << expected.toString()
                               << " but found " << actual.toString() << " - " << msg;
}

ReplSetHeartbeatResponse ReplCoordHBV1Test::receiveHeartbeatFrom(const ReplSetConfig& rsConfig,
                                                                 int sourceId,
                                                                 const HostAndPort& source) {
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setConfigVersion(rsConfig.getConfigVersion());
    hbArgs.setConfigTerm(rsConfig.getConfigTerm());
    hbArgs.setSetName(rsConfig.getReplSetName());
    hbArgs.setSenderHost(source);
    hbArgs.setSenderId(sourceId);
    hbArgs.setTerm(1);
    ASSERT(hbArgs.isInitialized());

    ReplSetHeartbeatResponse response;
    ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &response));
    return response;
}

TEST_F(ReplCoordHBV1Test,
       NodeJoinsExistingReplSetWhenReceivingAConfigContainingTheNodeViaHeartbeat) {
    setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
    ReplSetConfig rsConfig = assertMakeRSConfig(BSON("_id"
                                                     << "mySet"
                                                     << "version" << 3 << "members"
                                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                              << "h1:1")
                                                                   << BSON("_id" << 2 << "host"
                                                                                 << "h2:1")
                                                                   << BSON("_id" << 3 << "host"
                                                                                 << "h3:1"))
                                                     << "protocolVersion" << 1));
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
    ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
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
    OperationContextNoop opCtx;
    ReplSetConfig storedConfig;
    ASSERT_OK(storedConfig.initialize(
        unittest::assertGet(getExternalState()->loadLocalConfigDocument(&opCtx))));
    ASSERT_OK(storedConfig.validate());
    ASSERT_EQUALS(3, storedConfig.getConfigVersion());
    ASSERT_EQUALS(3, storedConfig.getNumMembers());
    exitNetwork();

    ASSERT_TRUE(getExternalState()->threadsStarted());
}

class ReplCoordHBV1ReconfigTest : public ReplCoordHBV1Test {
public:
    void setUp() {
        setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
        BSONObj configBson = BSON("_id"
                                  << "mySet"
                                  << "version" << initConfigVersion << "term" << initConfigTerm
                                  << "members" << members << "protocolVersion" << 1);
        ReplSetConfig rsConfig = assertMakeRSConfig(configBson);
        assertStartSuccess(configBson, HostAndPort("h2", 1));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        // Black hole initial heartbeat requests.
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        net->blackHole(net->getNextReadyRequest());
        net->blackHole(net->getNextReadyRequest());
        net->exitNetwork();
    }

    BSONObj makeConfigObj(long long version, boost::optional<long long> term) {
        BSONObjBuilder bob;
        bob.appendElements(BSON("_id"
                                << "mySet"
                                << "version" << version << "members" << members << "protocolVersion"
                                << 1));
        if (term) {
            bob.append("term", *term);
        }
        return bob.obj();
    }

    ReplSetConfig makeRSConfigWithVersionAndTerm(long long version, long long term) {
        return assertMakeRSConfig(makeConfigObj(version, term));
    }

    int initConfigVersion = 2;
    int initConfigTerm = 2;
    long long UninitializedTerm = OpTime::kUninitializedTerm;
    BSONArray members = BSON_ARRAY(BSON("_id" << 1 << "host"
                                              << "h1:1")
                                   << BSON("_id" << 2 << "host"
                                                 << "h2:1")
                                   << BSON("_id" << 3 << "host"
                                                 << "h3:1"));
};


TEST_F(ReplCoordHBV1ReconfigTest,
       NodeSchedulesHeartbeatToFetchConfigIfItHearsAboutConfigWithNewerVersionAndWillInstallIt) {
    // Config with newer version and same term.
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm((initConfigVersion + 1), initConfigTerm);

    // Receive a heartbeat request that tells us about a newer config.
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    getNet()->enterNetwork();
    ReplSetHeartbeatArgsV1 hbArgs;
    auto noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& hbrequest = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);
    ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(initConfigVersion, hbArgs.getConfigVersion());
    ASSERT_EQUALS(initConfigTerm, hbArgs.getConfigTerm());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());

    // Construct the heartbeat response containing the newer config.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    // Add the raw config object.
    responseBuilder << "config" << makeConfigObj(initConfigVersion + 1, initConfigTerm);
    auto origResObj = responseBuilder.obj();

    // Schedule and deliver the heartbeat response.
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(origResObj));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfig().getConfigVersion(), initConfigVersion + 1);
    ASSERT_EQ(getReplCoord()->getConfig().getConfigTerm(), initConfigTerm);
}

TEST_F(ReplCoordHBV1ReconfigTest,
       NodeSchedulesHeartbeatToFetchConfigIfItHearsAboutConfigWithNewerTermAndWillInstallIt) {
    // Config with newer term and same version.
    ReplSetConfig rsConfig = makeRSConfigWithVersionAndTerm(initConfigVersion, initConfigTerm + 1);

    // Receive a heartbeat request that tells us about a newer config.
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    getNet()->enterNetwork();
    ReplSetHeartbeatArgsV1 hbArgs;
    auto noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& hbrequest = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);
    ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(initConfigVersion, hbArgs.getConfigVersion());
    ASSERT_EQUALS(initConfigTerm, hbArgs.getConfigTerm());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());

    // Construct the heartbeat response containing the newer config.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    // Add the raw config object.
    responseBuilder << "config" << makeConfigObj(initConfigVersion, initConfigTerm + 1);
    auto origResObj = responseBuilder.obj();

    // Schedule and deliver the heartbeat response.
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(origResObj));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfig().getConfigTerm(), initConfigTerm + 1);
}

TEST_F(ReplCoordHBV1ReconfigTest,
       NodeShouldntScheduleHeartbeatToFetchConfigIfItHearsAboutSameConfig) {
    // Config with same term and same version. Shouldn't schedule any heartbeats.
    receiveHeartbeatFrom(getReplCoord()->getReplicaSetConfig_forTest(), 1, HostAndPort("h1", 1));
    getNet()->enterNetwork();
    ASSERT_FALSE(getNet()->hasReadyRequests());
}

TEST_F(
    ReplCoordHBV1ReconfigTest,
    NodeSchedulesHeartbeatToFetchConfigIfItHearsAboutConfigWithNewerTermAndLowerVersionAndWillInstallIt) {
    // Config with newer term and lower version.
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm((initConfigVersion - 1), (initConfigTerm + 1));

    // Receive a heartbeat request that tells us about a newer config.
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    getNet()->enterNetwork();
    ReplSetHeartbeatArgsV1 hbArgs;
    auto noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& hbrequest = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);
    ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(initConfigVersion, hbArgs.getConfigVersion());
    ASSERT_EQUALS(initConfigTerm, hbArgs.getConfigTerm());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());

    // Construct the heartbeat response containing the newer config.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    // Add the raw config object.
    responseBuilder << "config" << makeConfigObj((initConfigVersion - 1), (initConfigTerm + 1));
    auto origResObj = responseBuilder.obj();

    // Schedule and deliver the heartbeat response.
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(origResObj));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfig().getConfigVersion(), (initConfigVersion - 1));
    ASSERT_EQ(getReplCoord()->getConfig().getConfigTerm(), (initConfigTerm + 1));
}

TEST_F(
    ReplCoordHBV1ReconfigTest,
    NodeSchedulesHeartbeatToFetchConfigIfItHearsAboutConfigWithNewerVersionAndUninitializedTermAndWillInstallIt) {
    // Config with version and uninitialized term.
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm(initConfigVersion + 1, UninitializedTerm);

    // Receive a heartbeat request that tells us about a newer config.
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    getNet()->enterNetwork();
    ReplSetHeartbeatArgsV1 hbArgs;
    auto noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& hbrequest = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);
    ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(initConfigVersion, hbArgs.getConfigVersion());
    ASSERT_EQUALS(initConfigTerm, hbArgs.getConfigTerm());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());

    // Construct the heartbeat response containing the newer config.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    // Add the raw config object.
    responseBuilder << "config" << makeConfigObj((initConfigVersion + 1), UninitializedTerm);
    auto origResObj = responseBuilder.obj();

    // Schedule and deliver the heartbeat response.
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(origResObj));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfig().getConfigVersion(), (initConfigVersion + 1));
    ASSERT_EQ(getReplCoord()->getConfig().getConfigTerm(), UninitializedTerm);
}

TEST_F(ReplCoordHBV1ReconfigTest,
       NodeSchedulesHeartbeatToFetchNewerConfigAndInstallsConfigWithNoTermField) {
    // Config with newer version.
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm(initConfigVersion + 1, UninitializedTerm);

    // Receive a heartbeat request that tells us about a newer config.
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    getNet()->enterNetwork();
    ReplSetHeartbeatArgsV1 hbArgs;
    auto noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& hbrequest = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);
    ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(initConfigVersion, hbArgs.getConfigVersion());
    ASSERT_EQUALS(initConfigTerm, hbArgs.getConfigTerm());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());

    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    auto origResObj = responseBuilder.obj();

    // Construct a heartbeat response object that omits the top-level 't' field and the 'term' field
    // from the config object. This simulates the case of receiving a heartbeat response from a 4.2
    // node.
    BSONObjBuilder finalRes;
    for (auto field : origResObj.getFieldNames<std::set<std::string>>()) {
        if (field == "t") {
            continue;
        } else if (field == "config") {
            finalRes.append("config", makeConfigObj(initConfigVersion + 1, boost::none /* term */));
        } else {
            finalRes.append(origResObj[field]);
        }
    }

    // Make sure the response has no term fields.
    auto finalResObj = finalRes.obj();
    ASSERT_FALSE(finalResObj.hasField("t"));
    ASSERT_TRUE(finalResObj.hasField("config"));
    ASSERT_TRUE(finalResObj["config"].isABSONObj());
    ASSERT_FALSE(finalResObj.getObjectField("config").hasField("term"));

    // Schedule and deliver the heartbeat response.
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(finalResObj));
    getNet()->runReadyNetworkOperations();

    // We should have installed the newer config, even though it had no term attached.
    auto myConfig = getReplCoord()->getConfig();
    ASSERT_EQ(myConfig.getConfigVersion(), initConfigVersion + 1);
    ASSERT_EQ(myConfig.getConfigTerm(), UninitializedTerm);
}

TEST_F(ReplCoordHBV1Test, AwaitIsMasterReturnsResponseOnReconfigViaHeartbeat) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A reconfig should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;
    auto opCtx = makeOperationContext();
    // awaitIsMasterResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getIsMasterThread([&] {
        auto response = getReplCoord()->awaitIsMasterResponse(
            opCtx.get(), {}, currentTopologyVersion, deadline);
        auto topologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedProcessId);

        // Ensure the isMasterResponse contains the newly added node.
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(3, hosts.size());
        ASSERT_EQUALS("node3", hosts[2].host());
    });

    setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "mySet"
                                << "version" << 3 << "protocolVersion" << 1 << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "node1:12345"
                                                         << "priority" << 3)
                                              << BSON("_id" << 1 << "host"
                                                            << "node2:12345")
                                              << BSON("_id" << 2 << "host"
                                                            << "node3:12345"))));
    const Date_t startDate = getNet()->now();

    enterNetwork();
    NetworkInterfaceMock* net = getNet();
    ASSERT_FALSE(net->hasReadyRequests());
    exitNetwork();
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("node2", 12345));

    enterNetwork();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    net->scheduleResponse(
        noi, startDate + Milliseconds(200), makeResponseStatus(responseBuilder.obj()));
    assertRunUntil(startDate + Milliseconds(200));

    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    noi = net->getNextReadyRequest();

    exitNetwork();
    getIsMasterThread.join();
}

TEST_F(ReplCoordHBV1Test,
       ArbiterJoinsExistingReplSetWhenReceivingAConfigContainingTheArbiterViaHeartbeat) {
    setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "mySet"
                                << "version" << 3 << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1"
                                                            << "arbiterOnly" << true)
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1"))
                                << "protocolVersion" << 1));
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
    ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    net->scheduleResponse(
        noi, startDate + Milliseconds(200), makeResponseStatus(responseBuilder.obj()));
    assertRunUntil(startDate + Milliseconds(200));

    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    noi = net->getNextReadyRequest();

    assertMemberState(MemberState::RS_ARBITER);
    OperationContextNoop opCtx;
    ReplSetConfig storedConfig;
    ASSERT_OK(storedConfig.initialize(
        unittest::assertGet(getExternalState()->loadLocalConfigDocument(&opCtx))));
    ASSERT_OK(storedConfig.validate());
    ASSERT_EQUALS(3, storedConfig.getConfigVersion());
    ASSERT_EQUALS(3, storedConfig.getNumMembers());
    exitNetwork();

    ASSERT_FALSE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordHBV1Test,
       NodeDoesNotJoinExistingReplSetWhenReceivingAConfigNotContainingTheNodeViaHeartbeat) {
    // Tests that a node in RS_STARTUP will not transition to RS_REMOVED if it receives a
    // configuration that does not contain it.
    setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
    ReplSetConfig rsConfig = assertMakeRSConfig(BSON("_id"
                                                     << "mySet"
                                                     << "version" << 3 << "members"
                                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                              << "h1:1")
                                                                   << BSON("_id" << 2 << "host"
                                                                                 << "h2:1")
                                                                   << BSON("_id" << 3 << "host"
                                                                                 << "h3:1"))
                                                     << "protocolVersion" << 1));
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
    ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
    ASSERT_EQUALS(OpTime::kInitialTerm, hbArgs.getTerm());
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    net->scheduleResponse(
        noi, startDate + Milliseconds(50), makeResponseStatus(responseBuilder.obj()));
    assertRunUntil(startDate + Milliseconds(550));

    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    noi = net->getNextReadyRequest();

    assertMemberState(MemberState::RS_STARTUP, "2");
    OperationContextNoop opCtx;

    StatusWith<BSONObj> loadedConfig(getExternalState()->loadLocalConfigDocument(&opCtx));
    ASSERT_NOT_OK(loadedConfig.getStatus()) << loadedConfig.getValue();
    exitNetwork();
}

TEST_F(ReplCoordHBV1Test,
       NodeReturnsNotYetInitializedInResponseToAHeartbeatReceivedPriorToAConfig) {
    // ensure that if we've yet to receive an initial config, we return NotYetInitialized
    init("mySet");
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setConfigVersion(3);
    hbArgs.setSetName("mySet");
    hbArgs.setSenderHost(HostAndPort("h1:1"));
    hbArgs.setSenderId(1);
    hbArgs.setTerm(1);
    ASSERT(hbArgs.isInitialized());

    ReplSetHeartbeatResponse response;
    Status status = getReplCoord()->processHeartbeatV1(hbArgs, &response);
    ASSERT_EQUALS(ErrorCodes::NotYetInitialized, status.code());
}

TEST_F(ReplCoordHBV1Test,
       NodeChangesToRecoveringStateWhenAllNodesRespondToHeartbeatsWithUnauthorized) {
    // Tests that a node that only has auth error heartbeats is recovering
    setMinimumLoggedSeverity(logv2::LogSeverity::Debug(3));
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    // process heartbeat
    enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    LOGV2(21492,
          "{request_target} processing {request_cmdObj}",
          "request_target"_attr = request.target.toString(),
          "request_cmdObj"_attr = request.cmdObj);
    getNet()->scheduleResponse(
        noi,
        getNet()->now(),
        makeResponseStatus(BSON("ok" << 0.0 << "errmsg"
                                     << "unauth'd"
                                     << "code" << ErrorCodes::Unauthorized)));

    if (request.target != HostAndPort("node2", 12345) &&
        request.cmdObj.firstElement().fieldNameStringData() != "replSetHeartbeat") {
        LOGV2_ERROR(21496,
                    "Black holing unexpected request to {request_target}: {request_cmdObj}",
                    "request_target"_attr = request.target,
                    "request_cmdObj"_attr = request.cmdObj);
        getNet()->blackHole(noi);
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    ASSERT_TRUE(getTopoCoord().getMemberState().recovering());
    assertMemberState(MemberState::RS_RECOVERING, "0");
}

TEST_F(ReplCoordHBV1Test, IgnoreTheContentsOfMetadataWhenItsReplicaSetIdDoesNotMatchOurs) {
    // Tests that a secondary node will not update its committed optime from the heartbeat metadata
    // if the replica set ID is inconsistent with the existing configuration.
    HostAndPort host2("node2:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host" << host2.toString()))
                            << "settings" << BSON("replicaSetId" << OID::gen()) << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto rsConfig = getReplCoord()->getConfig();

    // Prepare heartbeat response.
    OID unexpectedId = OID::gen();
    OpTime opTime{Timestamp{10, 10}, 10};
    RemoteCommandResponse heartbeatResponse(ErrorCodes::InternalError, "not initialized");
    {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(rsConfig.getReplSetName());
        hbResp.setState(MemberState::RS_PRIMARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
        hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});

        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1;
        hbResp.addToBSON(&responseBuilder);

        rpc::ReplSetMetadata metadata(opTime.getTerm(),
                                      {opTime, Date_t()},
                                      opTime,
                                      rsConfig.getConfigVersion(),
                                      0,
                                      unexpectedId,
                                      1,
                                      -1,
                                      true);
        uassertStatusOK(metadata.writeToMetadata(&responseBuilder));

        heartbeatResponse = makeResponseStatus(responseBuilder.obj());
    }

    // process heartbeat
    enterNetwork();
    auto net = getNet();
    while (net->hasReadyRequests()) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        if (request.target == host2 &&
            request.cmdObj.firstElement().fieldNameStringData() == "replSetHeartbeat") {
            LOGV2(21493,
                  "{request_target} processing {request_cmdObj}",
                  "request_target"_attr = request.target.toString(),
                  "request_cmdObj"_attr = request.cmdObj);
            net->scheduleResponse(noi, net->now(), heartbeatResponse);
        } else {
            LOGV2(21494,
                  "blackholing request to {request_target}: {request_cmdObj}",
                  "request_target"_attr = request.target.toString(),
                  "request_cmdObj"_attr = request.cmdObj);
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    exitNetwork();

    ASSERT_NOT_EQUALS(opTime, getReplCoord()->getLastCommittedOpTime());
    ASSERT_NOT_EQUALS(opTime.getTerm(), getTopoCoord().getTerm());

    BSONObjBuilder statusBuilder;
    ASSERT_OK(getReplCoord()->processReplSetGetStatus(
        &statusBuilder, ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic));
    auto statusObj = statusBuilder.obj();
    LOGV2(21495, "replica set status = {statusObj}", "statusObj"_attr = statusObj);

    ASSERT_EQ(mongo::Array, statusObj["members"].type());
    auto members = statusObj["members"].Array();
    ASSERT_EQ(2U, members.size());
    ASSERT_TRUE(members[1].isABSONObj());
    auto member = members[1].Obj();
    ASSERT_EQ(host2, HostAndPort(member["name"].String()));
    ASSERT_EQ(MemberState(MemberState::RS_DOWN).toString(),
              MemberState(member["state"].numberInt()).toString());
    ASSERT_EQ(member["lastHeartbeatMessage"].String(),
              std::string(str::stream()
                          << "replica set IDs do not match, ours: " << rsConfig.getReplicaSetId()
                          << "; remote node's: " << unexpectedId));
}

TEST_F(ReplCoordHBV1Test,
       LastCommittedOpTimeOnlyUpdatesFromHeartbeatWhenLastAppliedHasTheSameTerm) {
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto opTime1 = OpTime({10, 1}, 1);
    auto opTime2 = OpTime({11, 1}, 2);  // In higher term.
    auto commitPoint = OpTime({15, 1}, 2);
    replCoordSetMyLastAppliedOpTime(opTime1, Date_t() + Seconds(100));

    // Node 1 is the current primary. The commit point has a higher term than lastApplied.
    rpc::ReplSetMetadata metadata(
        2,                                                         // term
        {commitPoint, Date_t() + Seconds(commitPoint.getSecs())},  // committed OpTime
        commitPoint,                                               // visibleOpTime
        config.getConfigVersion(),
        0,
        {},     // replset id
        1,      // currentPrimaryIndex,
        1,      // currentSyncSourceIndex
        true);  // isPrimary

    auto net = getNet();
    BSONObjBuilder responseBuilder;
    ASSERT_OK(metadata.writeToMetadata(&responseBuilder));

    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(config.getConfigVersion());
    hbResp.setSetName(config.getReplSetName());
    hbResp.setState(MemberState::RS_PRIMARY);
    responseBuilder.appendElements(hbResp.toBSON());
    auto hbRespObj = responseBuilder.obj();
    {
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS(config.getMemberAt(1).getHostAndPort(), request.target);
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());
        ASSERT_EQUALS(2, getReplCoord()->getTerm());
    }

    // Update lastApplied, so commit point can be advanced.
    replCoordSetMyLastAppliedOpTime(opTime2, Date_t() + Seconds(100));
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_EQUALS(commitPoint, getReplCoord()->getLastCommittedOpTime());
    }
}

TEST_F(ReplCoordHBV1Test, LastCommittedOpTimeOnlyUpdatesFromHeartbeatIfNotInStartup) {
    // Ensure that the metadata is not processed if it is contained in a heartbeat response,
    // if we are in STARTUP2.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto lastAppliedOpTime = OpTime({11, 1}, 2);
    auto commitPoint = OpTime({15, 1}, 2);
    replCoordSetMyLastAppliedOpTime(lastAppliedOpTime, Date_t() + Seconds(100));

    // Node 1 is the current primary.
    rpc::ReplSetMetadata metadata(
        2,                                                         // term
        {commitPoint, Date_t() + Seconds(commitPoint.getSecs())},  // committed OpTime
        commitPoint,                                               // visibleOpTime
        config.getConfigVersion(),
        0,
        {},     // replset id
        1,      // currentPrimaryIndex,
        1,      // currentSyncSourceIndex
        true);  // isPrimary

    auto net = getNet();
    BSONObjBuilder responseBuilder;
    ASSERT_OK(metadata.writeToMetadata(&responseBuilder));

    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(config.getConfigVersion());
    hbResp.setSetName(config.getReplSetName());
    hbResp.setState(MemberState::RS_PRIMARY);
    responseBuilder.appendElements(hbResp.toBSON());
    auto hbRespObj = responseBuilder.obj();
    // Last committed optime should not advance in STARTUP2.
    ASSERT_EQ(getReplCoord()->getMemberState(), MemberState::RS_STARTUP2);
    {
        net->enterNetwork();
        ASSERT_TRUE(net->hasReadyRequests());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS(config.getMemberAt(1).getHostAndPort(), request.target);
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());
        ASSERT_EQUALS(2, getReplCoord()->getTerm());
    }

    // Set follower mode to SECONDARY so commit point can be advanced through heartbeats.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_EQUALS(commitPoint, getReplCoord()->getLastCommittedOpTime());
    }
}

/**
 * Test a concurrent stepdown and reconfig. The stepdown is triggered by a heartbeat response with
 * a higher term, the reconfig is triggered either by a heartbeat with a new config, or by a user
 * replSetReconfig command.
 *
 * In setUp, the replication coordinator is initialized so "self" is the primary of a 3-node set.
 * The coordinator schedules heartbeats to the other nodes but this test doesn't respond to those
 * heartbeats. Instead, it creates heartbeat responses that have no associated requests, and injects
 * the responses via handleHeartbeatResponse_forTest.
 *
 * Each subclass of HBStepdownAndReconfigTest triggers some sequence of stepdown and reconfig steps.
 * The exact sequences are nondeterministic, since we don't use failpoints or NetworkInterfaceMock
 * to force a specific order.
 *
 * Tests assert that stepdown via heartbeat completed, and the tests that send the new config via
 * heartbeat assert that the new config was stored. Tests that send the new config with the
 * replSetReconfig command don't check that it was stored; if the stepdown finished first then the
 * replSetReconfig was rejected with a NotMaster error.
 */
class HBStepdownAndReconfigTest : public ReplCoordHBV1Test {
protected:
    void setUp() override;
    void tearDown() override;
    void sendHBResponse(int targetIndex,
                        long long term,
                        long long configVersion,
                        long long configTerm,
                        bool includeConfig);
    void sendHBResponseWithNewConfig();
    void sendHBResponseWithNewTerm();
    Future<void> startReconfigCommand();
    void assertSteppedDown();
    void assertConfigStored();

    const BSONObj _initialConfig = BSON("_id"
                                        << "mySet"
                                        << "version" << 2 << "members"
                                        << BSON_ARRAY(BSON("host"
                                                           << "node0:12345"
                                                           << "_id" << 0)
                                                      << BSON("host"
                                                              << "node1:12345"
                                                              << "_id" << 1)
                                                      << BSON("host"
                                                              << "node2:12345"
                                                              << "_id" << 2))
                                        << "protocolVersion" << 1);

    OpTime _commitPoint = OpTime(Timestamp(100, 1), 0);
    Date_t _wallTime = Date_t() + Seconds(100);
    std::unique_ptr<ThreadPool> _threadPool;
};

void HBStepdownAndReconfigTest::setUp() {
    ReplCoordHBV1Test::setUp();

    // We need one thread to run processReplSetReconfig, use a pool for convenience.
    _threadPool = std::make_unique<ThreadPool>(ThreadPool::Options());
    _threadPool->startup();

    assertStartSuccess(_initialConfig, HostAndPort("node0", 12345));


    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(_commitPoint, _wallTime);
    replCoordSetMyLastDurableOpTime(_commitPoint, _wallTime);
    simulateSuccessfulV1Election();

    // New term.
    ASSERT_EQUALS(1, replCoord->getTerm());
    _wallTime = _wallTime + Seconds(1);
    _commitPoint = OpTime(Timestamp(200, 2), 1);

    // To complete a reconfig from Config 1 to Config 2 requires:
    // Oplog Commitment: last write in previous Config 0 is majority-committed.
    // Config Replication: Config 2 gossipped by heartbeat response to majority of Config 2 members.
    //
    // Catch up all members to the same OpTime to ensure Oplog Commitment in all tests.
    // In tests that require it, we ensure Config Replication with acknowledgeReconfigCommand().
    for (auto i = 0; i < 3; ++i) {
        ASSERT_OK(replCoord->setLastAppliedOptime_forTest(2, i, _commitPoint, _wallTime));
        ASSERT_OK(replCoord->setLastDurableOptime_forTest(2, i, _commitPoint, _wallTime));
    }

    setMinimumLoggedSeverity(logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(2));
}

void HBStepdownAndReconfigTest::tearDown() {
    clearMinimumLoggedSeverity(logv2::LogComponent::kReplication);
    _threadPool.reset();
    ReplCoordHBV1Test::tearDown();
}

void HBStepdownAndReconfigTest::sendHBResponse(int targetIndex,
                                               long long term,
                                               long long configVersion,
                                               long long configTerm,
                                               bool includeConfig) {
    auto replCoord = getReplCoord();
    OpTime opTime(Timestamp(), 0);

    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setTerm(term);
    hbResp.setConfigVersion(configVersion);
    hbResp.setConfigTerm(configTerm);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t()});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t()});

    if (includeConfig) {
        auto configDoc = MutableDocument(Document(_initialConfig));
        configDoc["version"] = Value(configVersion);
        ReplSetConfig newConfig;
        ASSERT_OK(newConfig.initialize(configDoc.freeze().toBson()));
        hbResp.setConfig(newConfig);
    }

    BSONObjBuilder responseBuilder;
    responseBuilder.appendElements(hbResp.toBSON());
    replCoord->handleHeartbeatResponse_forTest(responseBuilder.obj(), targetIndex);
}

void HBStepdownAndReconfigTest::sendHBResponseWithNewConfig() {
    // Send a heartbeat response from a secondary, with newer config.
    sendHBResponse(2 /* targetIndex */,
                   1 /* term */,
                   3 /* configVersion */,
                   1 /* configTerm */,
                   true /* includeConfig */);
}

void HBStepdownAndReconfigTest::sendHBResponseWithNewTerm() {
    // Send a heartbeat response from a secondary, with higher term.
    sendHBResponse(1 /* targetIndex */,
                   2 /* term */,
                   2 /* configVersion */,
                   1 /* configTerm */,
                   false /* includeConfig */);
}

Future<void> HBStepdownAndReconfigTest::startReconfigCommand() {
    auto [promise, future] = makePromiseFuture<void>();

    // Send a user replSetReconfig command.
    auto coord = getReplCoord();
    auto newConfig = MutableDocument(Document(_initialConfig));
    newConfig["version"] = Value(3);
    ReplicationCoordinator::ReplSetReconfigArgs args{
        newConfig.freeze().toBson(), false /* force */
    };

    auto opCtx = ReplCoordHBV1Test::makeOperationContext();

    _threadPool->schedule(
        [promise = std::move(promise), coord, args, opCtx = std::move(opCtx)](Status) mutable {
            // Avoid the need to respond to quorum-check heartbeats sent to the other two members.
            // These heartbeats are sent *before* reconfiguring, they're distinct from the oplog
            // commitment and config replication checks.
            FailPointEnableBlock omitConfigQuorumCheck("omitConfigQuorumCheck");
            BSONObjBuilder result;
            auto status = Status::OK();
            try {
                // OK for processReplSetReconfig to return, throw NotMaster-like error, or succeed.
                status = coord->processReplSetReconfig(opCtx.get(), args, &result);
            } catch (const DBException&) {
                status = exceptionToStatus();
            }

            if (!status.isOK()) {
                ASSERT(ErrorCodes::isNotMasterError(status.code()));
                LOGV2(463817,
                      "processReplSetReconfig threw expected error",
                      "errorCode"_attr = status.code(),
                      "message"_attr = status.reason());
            }
            promise.emplaceValue();
        });

    return std::move(future);
}

void HBStepdownAndReconfigTest::assertSteppedDown() {
    LOGV2(463811, "Waiting for background jobs");
    // The clock is mocked, we don't actually sleep.
    sleepFor(Seconds(1));

    // Primary stepped down.
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    assertMemberState(MemberState::RS_SECONDARY);
}

void HBStepdownAndReconfigTest::assertConfigStored() {
    LOGV2(463812, "Waiting for background jobs");
    // The clock is mocked, we don't actually sleep.
    sleepFor(Seconds(1));

    ASSERT_EQUALS(ConfigVersionAndTerm(3, 1),
                  getReplCoord()->getConfig().getConfigVersionAndTerm());
}

TEST_F(HBStepdownAndReconfigTest, HBStepdownThenHBReconfig) {
    // A node has started to step down then learns about a new config via heartbeat.
    sendHBResponseWithNewTerm();
    sendHBResponseWithNewConfig();
    assertSteppedDown();
    assertConfigStored();
}

TEST_F(HBStepdownAndReconfigTest, HBReconfigThenHBStepdown) {
    // A node has started to reconfig then learns about a new term via heartbeat.
    sendHBResponseWithNewConfig();
    sendHBResponseWithNewTerm();
    assertSteppedDown();
    assertConfigStored();
}

TEST_F(HBStepdownAndReconfigTest, HBStepdownThenReconfigCommand) {
    // A node has started to step down then someone calls replSetReconfig.
    sendHBResponseWithNewTerm();
    auto future = startReconfigCommand();
    future.get();
    assertSteppedDown();
}

TEST_F(HBStepdownAndReconfigTest, ReconfigCommandThenHBStepdown) {
    // Someone calls replSetReconfig then the node learns about a new term via heartbeat.
    auto future = startReconfigCommand();
    sendHBResponseWithNewTerm();
    future.get();
    assertSteppedDown();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
