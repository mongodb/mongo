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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_config_gen.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using InNetworkGuard = NetworkInterfaceMock::InNetworkGuard;

auto makePrimaryHeartbeatResponseFrom(const ReplSetConfig& rsConfig,
                                      const std::string& setName = "mySet") {
    // Construct the heartbeat response containing the newer config.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName(setName);
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    // The smallest valid optime in PV1.
    OpTime opTime(Timestamp(1, 1), 0);
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
    hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
    BSONObjBuilder responseBuilder;
    responseBuilder << "ok" << 1;
    hbResp.addToBSON(&responseBuilder);
    // Add the raw config object.
    responseBuilder << "config" << rsConfig.toBSON();
    return responseBuilder.obj();
}

class ReplCoordHBV1Test : public ReplCoordTest {
protected:
    explicit ReplCoordHBV1Test() : ReplCoordTest(Options{}.useMockClock(true)) {}

    void assertMemberState(MemberState expected, std::string msg = "");
    ReplSetHeartbeatResponse receiveHeartbeatFrom(
        const ReplSetConfig& rsConfig,
        int sourceId,
        const HostAndPort& source,
        int term = 1,
        boost::optional<int> currentPrimaryId = boost::none);

    NetworkInterfaceMock::NetworkOperationIterator performSyncToFinishReconfigHeartbeat();

    void processResponseFromPrimary(const ReplSetConfig& config,
                                    long long version = -2,
                                    long long term = OpTime::kInitialTerm,
                                    const HostAndPort& target = HostAndPort{"h1", 1});
};

void ReplCoordHBV1Test::assertMemberState(const MemberState expected, std::string msg) {
    const MemberState actual = getReplCoord()->getMemberState();
    ASSERT(expected == actual) << "Expected coordinator to report state " << expected.toString()
                               << " but found " << actual.toString() << " - " << msg;
}

ReplSetHeartbeatResponse ReplCoordHBV1Test::receiveHeartbeatFrom(
    const ReplSetConfig& rsConfig,
    int sourceId,
    const HostAndPort& source,
    const int term,
    const boost::optional<int> currentPrimaryId) {
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setConfigVersion(rsConfig.getConfigVersion());
    hbArgs.setConfigTerm(rsConfig.getConfigTerm());
    hbArgs.setSetName(rsConfig.getReplSetName());
    hbArgs.setSenderHost(source);
    hbArgs.setSenderId(sourceId);
    hbArgs.setTerm(term);
    if (currentPrimaryId) {
        hbArgs.setPrimaryId(*currentPrimaryId);
    }
    ASSERT(hbArgs.isInitialized());

    ReplSetHeartbeatResponse response;
    ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &response));
    return response;
}

NetworkInterfaceMock::NetworkOperationIterator
ReplCoordHBV1Test::performSyncToFinishReconfigHeartbeat() {
    // Because the new config is stored using an out-of-band thread, we need to perform some
    // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
    // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
    // other network operations get scheduled during or before the reconfig, though this may
    // cease to be true in the future.
    return getNet()->getNextReadyRequest();
}

void ReplCoordHBV1Test::processResponseFromPrimary(const ReplSetConfig& config,
                                                   long long version,
                                                   long long term,
                                                   const HostAndPort& target) {
    NetworkInterfaceMock* net = getNet();
    const Date_t startDate = getNet()->now();

    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    ASSERT_EQUALS(target, request.target);
    ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    ASSERT_EQUALS("mySet", hbArgs.getSetName());
    ASSERT_EQUALS(version, hbArgs.getConfigVersion());
    ASSERT_EQUALS(term, hbArgs.getTerm());

    auto response = makePrimaryHeartbeatResponseFrom(config);
    net->scheduleResponse(noi, startDate + Milliseconds(200), makeResponseStatus(response));
    assertRunUntil(startDate + Milliseconds(200));
}

TEST_F(ReplCoordHBV1Test,
       NodeJoinsExistingReplSetWhenReceivingAConfigContainingTheNodeViaHeartbeat) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(3)};
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id" << "mySet"
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
    start();
    enterNetwork();
    assertMemberState(MemberState::RS_STARTUP);
    NetworkInterfaceMock* net = getNet();
    ASSERT_FALSE(net->hasReadyRequests());
    exitNetwork();
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    enterNetwork();

    processResponseFromPrimary(rsConfig);

    performSyncToFinishReconfigHeartbeat();

    assertMemberState(MemberState::RS_STARTUP2);
    auto opCtx{makeOperationContext()};
    auto storedConfig = ReplSetConfig::parse(
        unittest::assertGet(getExternalState()->loadLocalConfigDocument(opCtx.get())));
    ASSERT_OK(storedConfig.validate());
    ASSERT_EQUALS(3, storedConfig.getConfigVersion());
    ASSERT_EQUALS(3, storedConfig.getNumMembers());
    exitNetwork();

    ASSERT_TRUE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordHBV1Test, RestartingHeartbeatsShouldOnlyCancelScheduledHeartbeats) {
    auto replAllSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(3)};

    auto replConfigBson = BSON("_id" << "mySet"
                                     << "protocolVersion" << 1 << "version" << 1 << "members"
                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                              << "node1:12345")
                                                   << BSON("_id" << 2 << "host"
                                                                 << "node2:12345")
                                                   << BSON("_id" << 3 << "host"
                                                                 << "node3:12345")));

    assertStartSuccess(replConfigBson, HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->updateTerm_forTest(1, nullptr);
    ASSERT_EQ(getReplCoord()->getTerm(), 1);

    auto rsConfig = getReplCoord()->getConfig();

    enterNetwork();
    for (int j = 0; j < 2; ++j) {
        const auto noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& hbrequest = noi->getRequest();

        // Skip responding to node2's heartbeat request, so that it stays in SENT state.
        if (hbrequest.target == HostAndPort("node2", 12345)) {
            getNet()->blackHole(noi);
            continue;
        }

        // Respond to node3's heartbeat request so that we schedule a new heartbeat request that
        // stays in SCHEDULED state.
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        // The smallest valid optime in PV1.
        OpTime opTime(Timestamp(1, 1), 0);
        hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
        hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
        hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1;
        hbResp.addToBSON(&responseBuilder);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(responseBuilder.obj()));

        getNet()->runReadyNetworkOperations();
    }
    ASSERT_FALSE(getNet()->hasReadyRequests());
    exitNetwork();

    // Receive a request from node3 saying it's the primary, so that we restart scheduled
    // heartbeats.
    receiveHeartbeatFrom(rsConfig,
                         3 /* senderId */,
                         HostAndPort("node3", 12345),
                         1 /* term */,
                         3 /* currentPrimaryId */);

    enterNetwork();

    // Verify that only node3's heartbeat request was cancelled.
    ASSERT_TRUE(getNet()->hasReadyRequests());
    const auto noi = getNet()->getNextReadyRequest();
    // 'request' represents the request sent from self(node1) back to node3.
    const RemoteCommandRequest& request = noi->getRequest();
    ReplSetHeartbeatArgsV1 args;
    ASSERT_OK(args.initialize(request.cmdObj));
    ASSERT_EQ(request.target, HostAndPort("node3", 12345));
    ASSERT_EQ(args.getPrimaryId(), -1);
    // We don't need to respond.
    getNet()->blackHole(noi);

    // The heartbeat request for node2 should not have been cancelled, so there should not be any
    // more network ready requests.
    ASSERT_FALSE(getNet()->hasReadyRequests());
    exitNetwork();
}

TEST_F(ReplCoordHBV1Test,
       SecondaryReceivesHeartbeatRequestFromPrimaryWithDifferentPrimaryIdRestartsHeartbeats) {
    auto replAllSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(3)};

    auto replConfigBson = BSON("_id" << "mySet"
                                     << "protocolVersion" << 1 << "version" << 1 << "members"
                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                              << "node1:12345")
                                                   << BSON("_id" << 2 << "host"
                                                                 << "node2:12345")
                                                   << BSON("_id" << 3 << "host"
                                                                 << "node3:12345")));

    assertStartSuccess(replConfigBson, HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->updateTerm_forTest(1, nullptr);
    ASSERT_EQ(getReplCoord()->getTerm(), 1);

    auto rsConfig = getReplCoord()->getConfig();

    for (int j = 0; j < 2; ++j) {
        // Respond to the initial heartbeat request so that we schedule a new heartbeat request that
        // stays in SCHEDULED state.
        replyToReceivedHeartbeatV1();
    }

    // Verify that there are no further heartbeat requests, since the heartbeat requests should be
    // scheduled for the future.
    enterNetwork();
    ASSERT_FALSE(getNet()->hasReadyRequests());
    exitNetwork();

    // We're a secondary and we receive a request from node3 saying it's the primary.
    receiveHeartbeatFrom(rsConfig,
                         3 /* senderId */,
                         HostAndPort("node3", 12345),
                         1 /* term */,
                         3 /* currentPrimaryId */);

    enterNetwork();
    std::set<std::string> expectedHosts{"node2", "node3"};
    std::set<std::string> actualHosts;
    for (size_t i = 0; i < expectedHosts.size(); i++) {
        const auto noi = getNet()->getNextReadyRequest();
        // 'request' represents the request sent from self(node1) back to node3
        const RemoteCommandRequest& request = noi->getRequest();
        ReplSetHeartbeatArgsV1 args;
        ASSERT_OK(args.initialize(request.cmdObj));
        actualHosts.insert(request.target.host());
        ASSERT_EQ(args.getPrimaryId(), -1);
        // We don't need to respond.
        getNet()->blackHole(noi);
    }
    ASSERT(expectedHosts == actualHosts);
    ASSERT_FALSE(getNet()->hasReadyRequests());
    exitNetwork();

    // Heartbeat in a stale term shouldn't re-schedule heartbeats.
    receiveHeartbeatFrom(rsConfig,
                         3 /* senderId */,
                         HostAndPort("node3", 12345),
                         0 /* term */,
                         3 /* currentPrimaryId */);
    enterNetwork();
    ASSERT_FALSE(getNet()->hasReadyRequests());
    exitNetwork();
}

TEST_F(
    ReplCoordHBV1Test,
    SecondaryReceivesHeartbeatRequestFromSecondaryWithDifferentPrimaryIdDoesNotRestartHeartbeats) {
    auto replAllSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(3)};
    auto replConfigBson = BSON("_id" << "mySet"
                                     << "protocolVersion" << 1 << "version" << 1 << "members"
                                     << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                              << "node1:12345")
                                                   << BSON("_id" << 2 << "host"
                                                                 << "node2:12345")
                                                   << BSON("_id" << 3 << "host"
                                                                 << "node3:12345")));

    assertStartSuccess(replConfigBson, HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQ(getReplCoord()->getTerm(), 0);

    enterNetwork();
    // Ignore the first 2 messages.
    for (int j = 0; j < 2; ++j) {
        const auto noi = getNet()->getNextReadyRequest();
        noi->getRequest();
        getNet()->blackHole(noi);
    }
    exitNetwork();

    // Node 2 thinks 3 is the primary. We don't restart heartbeats for that.
    receiveHeartbeatFrom(getReplCoord()->getConfig(),
                         2 /* senderId */,
                         HostAndPort("node3", 12345),
                         0 /* term */,
                         3 /* currentPrimaryId */);

    {
        enterNetwork();
        ASSERT_FALSE(getNet()->hasReadyRequests());
        exitNetwork();
    }
}
class ReplCoordHBV1SplitConfigTest : public ReplCoordHBV1Test {
public:
    void startUp(const std::string& hostAndPort) {
        ReplSettings settings;
        settings.setReplSetString(_replSetName);
        init(settings);

        BSONObj configBson =
            BSON("_id" << _replSetName << "version" << _configVersion << "term" << _configTerm
                       << "members" << _members << "protocolVersion" << 1);
        ReplSetConfig rsConfig = assertMakeRSConfig(configBson);
        assertStartSuccess(configBson, HostAndPort(hostAndPort));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        // Black hole initial heartbeat requests.
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();

        // Ignore the initial heartbeat requests sent to each of the 5 other nodes of this replica
        // set.
        net->blackHole(net->getNextReadyRequest());
        net->blackHole(net->getNextReadyRequest());
        net->blackHole(net->getNextReadyRequest());
        net->blackHole(net->getNextReadyRequest());
        net->blackHole(net->getNextReadyRequest());

        net->exitNetwork();
    }

    NetworkInterfaceMock::NetworkOperationIterator validateNextRequest(const std::string& target,
                                                                       const std::string& setName,
                                                                       const int configVersion,
                                                                       const int termVersion) {
        ASSERT(getNet()->hasReadyRequests());

        ReplSetHeartbeatArgsV1 hbArgs;
        auto noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& hbrequest = noi->getRequest();

        if (!target.empty()) {
            // We might not know the exact target as ordering might change. In that case, simply
            // validate the content of the requests and ignore to which node it's sent.
            ASSERT_EQUALS(HostAndPort(target, 1), hbrequest.target);
        }
        ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
        ASSERT_EQUALS(setName, hbArgs.getSetName());
        ASSERT_EQUALS(configVersion, hbArgs.getConfigVersion());
        ASSERT_EQUALS(termVersion, hbArgs.getConfigTerm());

        return noi;
    }

    BSONObj makeConfigObj(long long version, boost::optional<long long> term) {
        BSONObjBuilder bob;
        bob.appendElements(BSON("_id" << "mySet"
                                      << "version" << version << "members" << _members
                                      << "protocolVersion" << 1 << "settings"
                                      << BSON("replicaSetId" << OID::gen())));
        if (term) {
            bob.append("term", *term);
        }
        return bob.obj();
    }

    ReplSetConfig makeRSConfigWithVersionAndTerm(long long version, long long term) {
        return assertMakeRSConfig(makeConfigObj(version, term));
    }

    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kDefault,
                                                       logv2::LogSeverity::Debug(3)};

    int _configVersion = 2;
    int _configTerm = 2;

    BSONArray _members = BSON_ARRAY(
        BSON("_id" << 1 << "host"
                   << "h1:1")
        << BSON("_id" << 2 << "host"
                      << "h2:1")
        << BSON("_id" << 3 << "host"
                      << "h3:1")
        << BSON("_id" << 4 << "host"
                      << "h4:1"
                      << "votes" << 0 << "priority" << 0 << "tags" << BSON("recip" << "tag2"))
        << BSON("_id" << 5 << "host"
                      << "h5:1"
                      << "votes" << 0 << "priority" << 0 << "tags" << BSON("recip" << "tag2"))
        << BSON("_id" << 6 << "host"
                      << "h6:1"
                      << "votes" << 0 << "priority" << 0 << "tags" << BSON("recip" << "tag2")));

protected:
    const std::string _replSetName{"mySet"};
};

TEST_F(ReplCoordHBV1SplitConfigTest, RejectMismatchedSetNameInHeartbeatResponse) {
    const std::string otherNode{"h4:1"};
    startUp(otherNode);

    // Receive a heartbeat request that tells us about a config with a newer version
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm((_configVersion + 1), (_configTerm + 1));
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    {
        InNetworkGuard guard(getNet());

        // The received heartbeat has a greater (term, version), so verify that the next request
        // targets that host to retrieve the new config.
        auto noi = validateNextRequest("h1", _replSetName, _configVersion, _configTerm);

        // Schedule a heartbeat response which reports the higher (term, version) but wrong setName
        auto response = [&]() {
            auto newConfig =
                ReplSetConfig::parse(makeConfigObj((_configVersion + 1), (_configTerm + 1)));
            return makePrimaryHeartbeatResponseFrom(newConfig, "differentSetName");
        }();

        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
        getNet()->runReadyNetworkOperations();
    }

    // Validate that the recipient has rejected the heartbeat response
    ASSERT_EQ(getReplCoord()->getConfigVersion(), _configVersion);
    ASSERT_EQ(getReplCoord()->getConfigTerm(), _configTerm);
}

class ReplCoordHBV1ReconfigTest : public ReplCoordHBV1Test {
public:
    void setUp() override {
        BSONObj configBson =
            BSON("_id" << "mySet"
                       << "version" << initConfigVersion << "term" << initConfigTerm << "members"
                       << members << "protocolVersion" << 1);
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
        bob.appendElements(BSON("_id" << "mySet"
                                      << "version" << version << "members" << members
                                      << "protocolVersion" << 1));
        if (term) {
            bob.append("term", *term);
        }
        return bob.obj();
    }

    ReplSetConfig makeRSConfigWithVersionAndTerm(long long version, long long term) {
        return assertMakeRSConfig(makeConfigObj(version, term));
    }

    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kDefault,
                                                       logv2::LogSeverity::Debug(3)};

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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Schedule and deliver the heartbeat response containing the newer config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigVersion(), initConfigVersion + 1);
    ASSERT_EQ(getReplCoord()->getConfigTerm(), initConfigTerm);
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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Schedule and deliver the heartbeat response containing the new config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigTerm(), initConfigTerm + 1);
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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Schedule and deliver the heartbeat response containing the newer config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigVersion(), (initConfigVersion - 1));
    ASSERT_EQ(getReplCoord()->getConfigTerm(), (initConfigTerm + 1));
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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Schedule and deliver the heartbeat response containing the newer config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigVersion(), (initConfigVersion + 1));
    ASSERT_EQ(getReplCoord()->getConfigTerm(), UninitializedTerm);
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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    auto origResObj = makePrimaryHeartbeatResponseFrom(rsConfig);

    // Construct a heartbeat response object that omits the top-level 't' field and the 'term' field
    // from the config object. This simulates the case of receiving a heartbeat response from a 4.2
    // node.
    BSONObjBuilder finalRes;
    for (const auto& field : origResObj.getFieldNames<std::set<std::string>>()) {
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

TEST_F(ReplCoordHBV1ReconfigTest, ConfigWithTermAndVersionChangeOnlyDoesntCallIsSelf) {
    // Config with newer term and newer version.
    ReplSetConfig rsConfig =
        makeRSConfigWithVersionAndTerm(initConfigVersion + 1, initConfigTerm + 1);

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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Make isSelf not work.
    getExternalState()->clearSelfHosts();

    // Schedule and deliver the heartbeat response containing the newer config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigTerm(), initConfigTerm + 1);
    ASSERT_EQ(getReplCoord()->getConfigVersion(), initConfigVersion + 1);

    // If we couldn't find ourselves in the config, we're REMOVED.  That means we made an
    // unnecessary isSelf call which failed because we cleared selfHosts.
    ASSERT(getReplCoord()->getMemberState().secondary())
        << "Member state is actually " << getReplCoord()->getMemberState();
}

TEST_F(ReplCoordHBV1ReconfigTest, ConfigWithSignificantChangeDoesCallIsSelf) {
    // Config with members 1 (self) and 2 swapped.
    ReplSetConfig rsConfig = makeRSConfigWithVersionAndTerm(initConfigVersion + 1, initConfigTerm);
    auto mutableConfig = rsConfig.getMutable();
    auto members = mutableConfig.getMembers();
    std::swap(members[1], members[2]);
    mutableConfig.setMembers(members);
    rsConfig = ReplSetConfig(std::move(mutableConfig));

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
    ASSERT_EQUALS(initConfigTerm, hbArgs.getTerm());

    // Schedule and deliver the heartbeat response containing the newer config.
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
    getNet()->runReadyNetworkOperations();

    ASSERT_EQ(getReplCoord()->getConfigTerm(), initConfigTerm);
    ASSERT_EQ(getReplCoord()->getConfigVersion(), initConfigVersion + 1);

    // We should remain secondary.
    ASSERT(getReplCoord()->getMemberState().secondary())
        << "Member state is actually " << getReplCoord()->getMemberState();

    // We should be the member in slot 2, not slot 1.
    ASSERT_NE(rsConfig.getMemberAt(1).getId().getData(), getReplCoord()->getMyId());
    ASSERT_EQ(rsConfig.getMemberAt(2).getId().getData(), getReplCoord()->getMyId());
}

TEST_F(ReplCoordHBV1ReconfigTest, ScheduleImmediateHeartbeatToSpeedUpConfigCommitment) {
    // Prepare a config which is newer than the installed config
    ReplSetConfig rsConfig = [&]() {
        auto mutableConfig =
            makeRSConfigWithVersionAndTerm(initConfigVersion + 1, initConfigTerm + 1).getMutable();
        ReplSetConfigSettings settings;
        settings.setHeartbeatIntervalMillis(10000);
        mutableConfig.setSettings(settings);
        return ReplSetConfig(std::move(mutableConfig));
    }();

    // Receive the newer config from the primary
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h2", 1));
    {
        InNetworkGuard guard(getNet());
        auto noi = getNet()->getNextReadyRequest();
        auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
        getNet()->runReadyNetworkOperations();

        // Ignore the immediate heartbeats sent to secondaries as part of the reconfig process
        getNet()->blackHole(getNet()->getNextReadyRequest());
        getNet()->blackHole(getNet()->getNextReadyRequest());
    }

    // Receive a heartbeat from secondary with the updated configVersionAndTerm before the primary
    // has updated its in-memory MemberData for this secondary
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    // Verify that we schedule an _immediate_ heartbeat to the node with old config
    {
        InNetworkGuard guard(getNet());
        ASSERT_TRUE(getNet()->hasReadyRequests());
        auto noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& hbrequest = noi->getRequest();
        ASSERT_EQUALS(HostAndPort("h1", 1), hbrequest.target);

        ReplSetHeartbeatArgsV1 hbArgs;
        ASSERT_OK(hbArgs.initialize(hbrequest.cmdObj));
        ASSERT_EQUALS("mySet", hbArgs.getSetName());
        ASSERT_EQUALS(rsConfig.getConfigVersion(), hbArgs.getConfigVersion());
        ASSERT_EQUALS(rsConfig.getConfigTerm(), hbArgs.getConfigTerm());

        // Schedule a response to the immediate heartbeat check which returns the config
        auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));
        getNet()->runReadyNetworkOperations();
    }

    // Now receive a heartbeat from the same secondary with the updated configVersionAndTerm after
    // the primary has updated its in-memory MemberData for that node
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    // Verify that we DO NOT schedule an immediate heartbeat to the node
    {
        InNetworkGuard guard(getNet());
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }
}

TEST_F(ReplCoordHBV1ReconfigTest, FindOwnHostForHeartbeatReconfigQuick) {
    // Prepare a config which is newer than the installed config
    ReplSetConfig newConfig = [&]() {
        auto mutableConfig =
            makeRSConfigWithVersionAndTerm(initConfigVersion + 1, initConfigTerm + 1).getMutable();
        ReplSetConfigSettings settings;
        settings.setHeartbeatIntervalMillis(10000);
        mutableConfig.setSettings(settings);
        return ReplSetConfig(std::move(mutableConfig));
    }();

    // Receive the newer config from the primary
    receiveHeartbeatFrom(newConfig, 1, HostAndPort("h2", 1));
    {
        InNetworkGuard guard(getNet());
        auto noi = getNet()->getNextReadyRequest();
        auto response = makePrimaryHeartbeatResponseFrom(newConfig);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(response));

        unittest::LogCaptureGuard logs;
        getNet()->runReadyNetworkOperations();
        logs.stop();
        ASSERT_EQUALS(1, logs.countTextContaining("Was able to quickly find new index in config"));
    }
}

TEST_F(ReplCoordHBV1Test, RejectHeartbeatReconfigDuringElection) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplicationHeartbeats, logv2::LogSeverity::Debug(1)};

    auto term = 1;
    auto version = 1;
    auto members = BSON_ARRAY(member(1, "h1:1") << member(2, "h2:1") << member(3, "h3:1"));
    auto configObj = configWithMembers(version, term, members);
    assertStartSuccess(configObj, {"h1", 1});

    OpTime time1(Timestamp(100, 1), 0);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(time1, getNet()->now());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    simulateEnoughHeartbeatsForAllNodesUp();
    simulateSuccessfulDryRun();

    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    // Attach a config with a higher version and the same term.
    ReplSetConfig rsConfig = assertMakeRSConfig(configWithMembers(version + 1, term, members));
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfig(rsConfig);
    hbResp.setAppliedOpTimeAndWallTime({time1, getNet()->now()});
    hbResp.setWrittenOpTimeAndWallTime({time1, getNet()->now()});
    hbResp.setDurableOpTimeAndWallTime({time1, getNet()->now()});
    auto hbRespObj = (BSONObjBuilder(hbResp.toBSON()) << "ok" << 1).obj();

    unittest::LogCaptureGuard logs;
    getReplCoord()->handleHeartbeatResponse_forTest(hbRespObj, 1);
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    logs.stop();
    ASSERT_EQUALS(
        1,
        logs.countTextContaining("Not scheduling a heartbeat reconfig when running for election"));

    auto net = getNet();
    net->enterNetwork();
    while (net->hasReadyRequests()) {
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        LOGV2(482571, "processing", "to"_attr = request.target, "cmd"_attr = request.cmdObj);
        if (request.cmdObj.firstElementFieldNameStringData() != "replSetRequestVotes") {
            net->blackHole(noi);
        } else {
            auto response = BSON("ok" << 1 << "term" << term << "voteGranted" << true << "reason"
                                      << "");
            net->scheduleResponse(noi, net->now(), makeResponseStatus(response));
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();

    getReplCoord()->waitForElectionFinish_forTest();
    ASSERT(getReplCoord()->getMemberState().primary());
}

TEST_F(ReplCoordHBV1Test, AwaitHelloReturnsResponseOnReconfigViaHeartbeat) {
    init();
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))),
                       HostAndPort("node1", 12345));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(100, 1), 0),
                                                        Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A reconfig should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;
    auto opCtx = makeOperationContext();
    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        auto response =
            getReplCoord()->awaitHelloResponse(opCtx.get(), {}, currentTopologyVersion, deadline);
        auto topologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedProcessId);

        // Ensure the helloResponse contains the newly added node.
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(3, hosts.size());
        ASSERT_EQUALS("node3", hosts[2].host());
    });

    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(3)};
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id" << "mySet"
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
    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    net->scheduleResponse(noi, startDate + Milliseconds(200), makeResponseStatus(response));
    assertRunUntil(startDate + Milliseconds(200));

    performSyncToFinishReconfigHeartbeat();

    exitNetwork();
    getHelloThread.join();
}

TEST_F(ReplCoordHBV1Test,
       ArbiterJoinsExistingReplSetWhenReceivingAConfigContainingTheArbiterViaHeartbeat) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(3)};
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id" << "mySet"
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
    start();
    enterNetwork();
    assertMemberState(MemberState::RS_STARTUP);
    NetworkInterfaceMock* net = getNet();
    ASSERT_FALSE(net->hasReadyRequests());
    exitNetwork();
    receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

    enterNetwork();

    processResponseFromPrimary(rsConfig);

    performSyncToFinishReconfigHeartbeat();

    assertMemberState(MemberState::RS_ARBITER);
    auto opCtx{makeOperationContext()};
    auto storedConfig = ReplSetConfig::parse(
        unittest::assertGet(getExternalState()->loadLocalConfigDocument(opCtx.get())));
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
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(3)};
    ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id" << "mySet"
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

    auto response = makePrimaryHeartbeatResponseFrom(rsConfig);
    net->scheduleResponse(noi, startDate + Milliseconds(50), makeResponseStatus(response));
    assertRunUntil(startDate + Milliseconds(550));

    performSyncToFinishReconfigHeartbeat();

    assertMemberState(MemberState::RS_STARTUP, "2");
    auto opCtx{makeOperationContext()};
    StatusWith<BSONObj> loadedConfig(getExternalState()->loadLocalConfigDocument(opCtx.get()));
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
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kDefault,
                                                              logv2::LogSeverity::Debug(3)};
    assertStartSuccess(BSON("_id" << "mySet"
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
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 1 << "members"
                                  << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                           << "node1:12345")
                                                << BSON("_id" << 2 << "host" << host2.toString()))
                                  << "settings" << BSON("replicaSetId" << OID::gen())
                                  << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto rsConfig = getReplCoord()->getConfig();

    // Prepare heartbeat response.
    OID unexpectedId = OID::gen();
    OpTime opTime{Timestamp{10, 10}, 10};
    RemoteCommandResponse heartbeatResponse =
        RemoteCommandResponse::make_forTest(Status(ErrorCodes::InternalError, "not initialized"));
    {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(rsConfig.getReplSetName());
        hbResp.setState(MemberState::RS_PRIMARY);
        hbResp.setConfigVersion(rsConfig.getConfigVersion());
        hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
        hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
        hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});

        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1;
        hbResp.addToBSON(&responseBuilder);

        rpc::ReplSetMetadata metadata(opTime.getTerm(),
                                      {opTime, Date_t()},
                                      opTime,
                                      rsConfig.getConfigVersion(),
                                      0,
                                      unexpectedId,
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
    auto opCtx = makeOperationContext();
    ASSERT_OK(getReplCoord()->processReplSetGetStatus(
        opCtx.get(),
        &statusBuilder,
        ReplicationCoordinator::ReplSetGetStatusResponseStyle::kBasic));
    auto statusObj = statusBuilder.obj();
    LOGV2(21495, "replica set status = {statusObj}", "statusObj"_attr = statusObj);

    ASSERT_EQ(mongo::BSONType::array, statusObj["members"].type());
    auto members = statusObj["members"].Array();
    ASSERT_EQ(2U, members.size());
    ASSERT_TRUE(members[1].isABSONObj());
    auto member = members[1].Obj();
    ASSERT_EQ(host2, HostAndPort(member["name"].String()));
    ASSERT_EQ(MemberState(MemberState::RS_DOWN).toString(),
              MemberState(member["state"].numberInt()).toString());
}

TEST_F(ReplCoordHBV1Test,
       LastCommittedOpTimeOnlyUpdatesFromHeartbeatWhenLastAppliedHasTheSameTerm) {
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))
                                  << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto opTime1 = OpTime({10, 1}, 1);
    auto opTime2 = OpTime({11, 1}, 2);  // In higher term.
    auto commitPoint = OpTime({15, 1}, 2);
    replCoordSetMyLastWrittenOpTime(opTime1, Date_t() + Seconds(100));

    // Node 1 is the current primary. The commit point has a higher term than lastApplied.
    rpc::ReplSetMetadata metadata(
        2,                                                         // term
        {commitPoint, Date_t() + Seconds(commitPoint.getSecs())},  // committed OpTime
        commitPoint,                                               // visibleOpTime
        config.getConfigVersion(),
        0,
        {},     // replset id
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

    // Update lastWritten and lastApplied, so commit point can be advanced.
    replCoordSetMyLastWrittenOpTime(opTime2, Date_t() + Seconds(100));
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
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))
                                  << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto lastAppliedOpTime = OpTime({11, 1}, 2);
    auto commitPoint = OpTime({15, 1}, 2);
    replCoordSetMyLastWrittenOpTime(lastAppliedOpTime, Date_t() + Seconds(100));

    // Node 1 is the current primary.
    rpc::ReplSetMetadata metadata(
        2,                                                         // term
        {commitPoint, Date_t() + Seconds(commitPoint.getSecs())},  // committed OpTime
        commitPoint,                                               // visibleOpTime
        config.getConfigVersion(),
        0,
        {},     // replset id
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

TEST_F(ReplCoordHBV1Test, DoNotAttemptToUpdateLastCommittedOpTimeFromHeartbeatIfInRollbackState) {
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))
                                  << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto lastAppliedOpTime = OpTime({11, 1}, 2);
    auto commitPoint = OpTime({15, 1}, 2);
    replCoordSetMyLastWrittenOpTime(lastAppliedOpTime, Date_t() + Seconds(100));

    // Node 1 is the current primary.
    rpc::ReplSetMetadata metadata(
        2,                                                         // term
        {commitPoint, Date_t() + Seconds(commitPoint.getSecs())},  // committed OpTime
        commitPoint,                                               // visibleOpTime
        config.getConfigVersion(),
        0,
        {},     // replset id
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
        // We must take the RSTL in mode X before making state transitions.
        auto opCtx = makeOperationContext();
        ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        ASSERT_OK(getReplCoord()->setFollowerModeRollback(opCtx.get()));
    }

    // Last committed optime should not advance in ROLLBACK state.
    ASSERT_EQ(getReplCoord()->getMemberState(), MemberState::RS_ROLLBACK);
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

TEST_F(ReplCoordHBV1Test, handleHeartbeatResponseForTestEnqueuesValidHandle) {
    init();
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))),
                       HostAndPort("node1", 12345));

    OpTime opTime1(Timestamp(100, 1), 0);
    Date_t wallTime1 = Date_t() + Seconds(100);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    // Become primary.
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(opTime1, wallTime1);
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto failpoint = globalFailPointRegistry().find(
        "hangAfterTrackingNewHandleInHandleHeartbeatResponseForTest");
    auto timesEntered = failpoint->setMode(FailPoint::alwaysOn);

    // Prepare the test heartbeat response.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setTerm(1);
    hbResp.setConfigVersion(2);
    hbResp.setConfigTerm(1);
    hbResp.setAppliedOpTimeAndWallTime({opTime1, wallTime1});
    hbResp.setWrittenOpTimeAndWallTime({opTime1, wallTime1});
    hbResp.setDurableOpTimeAndWallTime({opTime1, wallTime1});
    auto hbRespObj = (BSONObjBuilder(hbResp.toBSON()) << "ok" << 1).obj();

    stdx::thread heartbeatReponseThread([&] {
        Client::initThread("handleHeartbeatResponseForTest",
                           getGlobalServiceContext()->getService());
        getReplCoord()->handleHeartbeatResponse_forTest(hbRespObj, 1 /* targetIndex */);
    });

    failpoint->waitForTimesEntered(timesEntered + 1);

    // Restarting all heartbeats includes canceling them first.
    // Canceling the dummy handle we just enqueued should *not* result in a crash.
    getReplCoord()->restartScheduledHeartbeats_forTest();

    failpoint->setMode(FailPoint::off, 0);
    heartbeatReponseThread.join();
}

TEST_F(ReplCoordHBV1Test, NotifiesExternalStateOfChangeOnlyWhenDataChanges) {
    unittest::MinimumLoggedSeverityGuard replLogSeverityGuard{logv2::LogComponent::kReplication,
                                                              logv2::LogSeverity::Debug(3)};
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id" << "mySet"
                                  << "term" << 1 << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << "node1:12345"
                                                            << "_id" << 0)
                                                << BSON("host" << "node2:12345"
                                                               << "_id" << 1))
                                  << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getLastCommittedOpTime());

    auto config = getReplCoord()->getConfig();

    auto net = getNet();
    ReplSetHeartbeatResponse hbResp;
    OpTimeAndWallTime appliedOpTimeAndWallTime = {OpTime({11, 1}, 1), Date_t::now()};
    OpTimeAndWallTime writtenOpTimeAndWallTime = {OpTime({11, 1}, 1), Date_t::now()};
    OpTimeAndWallTime durableOpTimeAndWallTime = {OpTime({10, 1}, 1), Date_t::now()};
    hbResp.setConfigVersion(config.getConfigVersion());
    hbResp.setConfigTerm(config.getConfigTerm());
    hbResp.setSetName(config.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setElectable(false);
    hbResp.setAppliedOpTimeAndWallTime(appliedOpTimeAndWallTime);
    hbResp.setWrittenOpTimeAndWallTime(writtenOpTimeAndWallTime);
    hbResp.setDurableOpTimeAndWallTime(durableOpTimeAndWallTime);
    auto hbRespObj = hbResp.toBSON();
    // First heartbeat, to set the stored data for the node.
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
    }

    // Second heartbeat, same as the first, should not trigger external notification.
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_FALSE(getExternalState()->getOtherMemberDataChanged());
    }

    // Change electability, should signal data changed.
    hbResp.setElectable(true);
    hbRespObj = hbResp.toBSON();
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_TRUE(getExternalState()->getOtherMemberDataChanged());
    }

    // Change applied optime, should signal data changed.
    appliedOpTimeAndWallTime.opTime = OpTime({11, 2}, 1);
    hbResp.setAppliedOpTimeAndWallTime(appliedOpTimeAndWallTime);
    hbRespObj = hbResp.toBSON();
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_TRUE(getExternalState()->getOtherMemberDataChanged());
    }

    // Change durable optime, should signal data changed.
    durableOpTimeAndWallTime.opTime = OpTime({10, 2}, 1);
    hbResp.setDurableOpTimeAndWallTime(durableOpTimeAndWallTime);
    hbRespObj = hbResp.toBSON();
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_TRUE(getExternalState()->getOtherMemberDataChanged());
    }

    // Change member state, should signal data changed.
    hbResp.setState(MemberState::RS_PRIMARY);
    hbRespObj = hbResp.toBSON();
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_TRUE(getExternalState()->getOtherMemberDataChanged());
    }

    // Change nothing again, should see no change.
    getExternalState()->clearOtherMemberDataChanged();
    {
        net->enterNetwork();
        net->runUntil(net->now() + config.getHeartbeatInterval());
        auto noi = net->getNextReadyRequest();
        auto& request = noi->getRequest();
        ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

        net->scheduleResponse(noi, net->now(), makeResponseStatus(hbRespObj));
        net->runReadyNetworkOperations();
        net->exitNetwork();

        ASSERT_FALSE(getExternalState()->getOtherMemberDataChanged());
    }
}

/**
 * Test a concurrent stepdown and reconfig. The stepdown is triggered by a heartbeat response
 * with a higher term, the reconfig is triggered either by a heartbeat with a new config, or by
 * a user replSetReconfig command.
 *
 * In setUp, the replication coordinator is initialized so "self" is the primary of a 3-node
 * set. The coordinator schedules heartbeats to the other nodes but this test doesn't respond to
 * those heartbeats. Instead, it creates heartbeat responses that have no associated requests,
 * and injects the responses via handleHeartbeatResponse_forTest.
 *
 * Each subclass of HBStepdownAndReconfigTest triggers some sequence of stepdown and reconfig
 * steps. The exact sequences are nondeterministic, since we don't use failpoints or
 * NetworkInterfaceMock to force a specific order.
 *
 * Tests assert that stepdown via heartbeat completed, and the tests that send the new config
 * via heartbeat assert that the new config was stored. Tests that send the new config with the
 * replSetReconfig command don't check that it was stored; if the stepdown finished first then
 * the replSetReconfig was rejected with a NotWritablePrimary error.
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

    const BSONObj _initialConfig = BSON("_id" << "mySet"
                                              << "version" << 2 << "members"
                                              << BSON_ARRAY(BSON("host" << "node0:12345"
                                                                        << "_id" << 0)
                                                            << BSON("host" << "node1:12345"
                                                                           << "_id" << 1)
                                                            << BSON("host" << "node2:12345"
                                                                           << "_id" << 2))
                                              << "protocolVersion" << 1);

    OpTime _commitPoint = OpTime(Timestamp(100, 1), 0);
    Date_t _wallTime = Date_t() + Seconds(100);
    std::unique_ptr<ThreadPool> _threadPool;

    unittest::MinimumLoggedSeverityGuard replLogSeverityGuard{logv2::LogComponent::kReplication,
                                                              logv2::LogSeverity::Debug(2)};
};

void HBStepdownAndReconfigTest::setUp() {
    ReplCoordHBV1Test::setUp();

    // We need one thread to run processReplSetReconfig, use a pool for convenience.
    _threadPool = std::make_unique<ThreadPool>(ThreadPool::Options());
    _threadPool->startup();

    assertStartSuccess(_initialConfig, HostAndPort("node0", 12345));


    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(_commitPoint, _wallTime);
    simulateSuccessfulV1Election();

    // New term.
    ASSERT_EQUALS(1, replCoord->getTerm());
    _wallTime = _wallTime + Seconds(1);
    _commitPoint = OpTime(Timestamp(200, 2), 1);

    // To complete a reconfig from Config 1 to Config 2 requires:
    // Oplog Commitment: last write in previous Config 0 is majority-committed.
    // Config Replication: Config 2 gossipped by heartbeat response to majority of Config 2
    // members.
    //
    // Catch up all members to the same OpTime to ensure Oplog Commitment in all tests.
    // In tests that require it, we ensure Config Replication with acknowledgeReconfigCommand().
    for (auto i = 0; i < 3; ++i) {
        ASSERT_OK(replCoord->setLastAppliedOptime_forTest(2, i, _commitPoint, _wallTime));
        ASSERT_OK(replCoord->setLastDurableOptime_forTest(2, i, _commitPoint, _wallTime));
    }
}

void HBStepdownAndReconfigTest::tearDown() {
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
    hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
    hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});
    hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds{1}});

    if (includeConfig) {
        auto configDoc = MutableDocument(Document(_initialConfig));
        configDoc["version"] = Value(configVersion);
        configDoc["term"] = Value(configTerm);
        auto newConfig = ReplSetConfig::parse(configDoc.freeze().toBson());
        hbResp.setConfig(newConfig);
    }

    replCoord->handleHeartbeatResponse_forTest(hbResp.toBSON(), targetIndex);
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
            // Avoid the need to respond to quorum-check heartbeats sent to the other two
            // members. These heartbeats are sent *before* reconfiguring, they're distinct from
            // the oplog commitment and config replication checks.
            FailPointEnableBlock omitConfigQuorumCheck("omitConfigQuorumCheck");
            BSONObjBuilder result;
            auto status = Status::OK();
            try {
                // OK for processReplSetReconfig to return, throw NotPrimary-like error, or
                // succeed.
                status = coord->processReplSetReconfig(opCtx.get(), args, &result);
            } catch (const DBException&) {
                status = exceptionToStatus();
            }

            if (!status.isOK()) {
                ASSERT(ErrorCodes::isNotPrimaryError(status.code()));
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
    LOGV2(463811, "Waiting for step down to complete");
    // Wait for step down to finish since it may be asynchronous.
    auto timeout = Milliseconds(5 * 60 * 1000);
    ASSERT_OK(getReplCoord()->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_SECONDARY, timeout));

    // Primary stepped down.
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    assertMemberState(MemberState::RS_SECONDARY);
}

void HBStepdownAndReconfigTest::assertConfigStored() {
    LOGV2(463812, "Waiting for config to be stored");
    // Wait for new config since it may be installed asynchronously.
    while (getReplCoord()->getConfigVersionAndTerm() < ConfigVersionAndTerm(3, 1)) {
        sleepFor(Milliseconds(10));
    }
    ASSERT_EQUALS(ConfigVersionAndTerm(3, 1), getReplCoord()->getConfigVersionAndTerm());
}

TEST_F(HBStepdownAndReconfigTest, HBStepdownThenHBReconfig) {
    // A node has started to step down then learns about a new config via heartbeat.
    sendHBResponseWithNewTerm();
    sendHBResponseWithNewConfig();
    assertSteppedDown();
    assertConfigStored();
}

TEST_F(HBStepdownAndReconfigTest, HBReconfigThenHBStepdown) {
    auto reconfigFp = globalFailPointRegistry().find("hangHeartbeatReconfigStore");
    auto timesEnteredReconfig = reconfigFp->setMode(FailPoint::alwaysOn);

    // A node has started to reconfig then learns about a new term via heartbeat.
    sendHBResponseWithNewConfig();

    // Wait for the mock repl executor to be in _heartbeatReconfigStore() and hang it there.
    reconfigFp->waitForTimesEntered(timesEnteredReconfig + 1);

    // Issue the heartbeat with the new term. This will schedule the stepDown task in the repl
    // executor, ensuring it is scheduled before the new config is installed and cancels all
    // heartbeats.
    sendHBResponseWithNewTerm();

    // Turn the failpoint off so that we can proceed with reconfig.
    reconfigFp->setMode(FailPoint::off);

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
