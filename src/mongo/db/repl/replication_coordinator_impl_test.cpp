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


#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/data_replicator_external_state_impl.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/replication_metrics.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shutdown_in_progress_quiesce_info.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using rpc::OplogQueryMetadata;
using rpc::ReplSetMetadata;
using unittest::assertGet;
using unittest::EnsureFCV;

typedef ReplicationCoordinator::ReplSetReconfigArgs ReplSetReconfigArgs;
// Helper class to wrap Timestamp as an OpTime with term 1.
struct OpTimeWithTermOne {
    OpTimeWithTermOne(unsigned int sec, unsigned int i) : timestamp(sec, i) {}
    operator OpTime() const {
        return OpTime(timestamp, 1);
    }

    operator boost::optional<OpTime>() const {
        return OpTime(timestamp, 1);
    }

    OpTime asOpTime() const {
        return this->operator mongo::repl::OpTime();
    }

    Timestamp timestamp;
};

OpTimeAndWallTime makeOpTimeAndWallTime(OpTime opTime, Date_t wallTime = Date_t()) {
    return {opTime, wallTime};
}

/**
 * Helper that kills an operation, taking the necessary locks.
 */
void killOperation(OperationContext* opCtx) {
    stdx::lock_guard<Client> lkClient(*opCtx->getClient());
    opCtx->getServiceContext()->killOperation(lkClient, opCtx);
}

std::shared_ptr<const repl::HelloResponse> awaitHelloWithNewOpCtx(
    ReplicationCoordinatorImpl* replCoord,
    TopologyVersion topologyVersion,
    const repl::SplitHorizon::Parameters& horizonParams,
    Date_t deadline) {
    auto newClient = getGlobalServiceContext()->makeClient("awaitIsHello");
    auto newOpCtx = newClient->makeOperationContext();
    return replCoord->awaitHelloResponse(newOpCtx.get(), horizonParams, topologyVersion, deadline);
}

TEST_F(ReplCoordTest, IsMasterIsFalseDuringStepdown) {
    BSONObj configObj = BSON("_id"
                             << "mySet"
                             << "version" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node2:12345"))
                             << "protocolVersion" << 1);
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);
    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(replCoord->getMemberState().primary());

    // Primary begins stepping down due to new term, but cannot finish.
    globalFailPointRegistry().find("blockHeartbeatStepdown")->setMode(FailPoint::alwaysOn);

    TopologyCoordinator::UpdateTermResult updateTermResult;
    replCoord->updateTerm_forTest(replCoord->getTerm() + 1, &updateTermResult);
    ASSERT(TopologyCoordinator::UpdateTermResult::kTriggerStepDown == updateTermResult);

    // Test that "ismaster" is immediately false, although "secondary" is not yet true.
    auto opCtx = makeOperationContext();
    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_TRUE(response->isConfigSet());
    BSONObj responseObj = response->toBSON();
    ASSERT_FALSE(responseObj["ismaster"].Bool());
    ASSERT_FALSE(responseObj["secondary"].Bool());
    ASSERT_FALSE(responseObj.hasField("isreplicaset"));

    globalFailPointRegistry().find("blockHeartbeatStepdown")->setMode(FailPoint::off);
}

TEST_F(ReplCoordTest, NodeEntersStartup2StateWhenStartingUpWithValidLocalConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_TRUE(getExternalState()->threadsStarted());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeEntersArbiterStateWhenStartingUpWithValidLocalConfigWhereItIsAnArbiter) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"
                                                     << "arbiterOnly" << true)
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_FALSE(getExternalState()->threadsStarted());
    ASSERT_EQUALS(MemberState::RS_ARBITER, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeEntersRemovedStateWhenStartingUpWithALocalConfigWhichLacksItAsAMember) {
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:54321"))),
                       HostAndPort("node3", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Locally stored replica set configuration does "
                                                    "not have a valid entry for the current node"));
    ASSERT_EQUALS(MemberState::RS_REMOVED, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeEntersRemovedStateWhenStartingUpWithALocalConfigContainingTheWrongSetName) {
    init("mySet");
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "notMySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("Local replica set configuration document set "
                                                    "name differs from command line set name"));
    ASSERT_EQUALS(MemberState::RS_REMOVED, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeEntersStartupStateWhenStartingUpWithNoLocalConfig) {
    startCapturingLogMessages();
    start();
    stopCapturingLogMessages();
    ASSERT_EQUALS(3, countTextFormatLogLinesContaining("Did not find local "));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeInitiateInServerlessMode) {
    ReplSettings settings;
    settings.setServerlessMode();

    ReplCoordTest::init(settings);
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    BSONObjBuilder result;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    auto config = getReplCoord()->getConfig();
    ASSERT_EQUALS("mySet", config.getReplSetName());
}

TEST_F(ReplCoordTest, NodeInitiateDifferentSetNames) {
    ReplCoordTest::init("cliSetName");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    BSONObjBuilder result;
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result));
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenInitiatedWithAnEmptyConfig) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(opCtx.get(), BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeReturnsAlreadyInitiatedWhenReceivingAnInitiateCommandAfterHavingAValidConfig) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    auto opCtx = makeOperationContext();

    // Starting uninitialized, show that we can perform the initiate behavior.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    ASSERT_TRUE(getExternalState()->threadsStarted());

    // Show that initiate fails after it has already succeeded.
    BSONObjBuilder result2;
    ASSERT_EQUALS(
        ErrorCodes::AlreadyInitialized,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result2));

    // Still in repl set mode, even after failed reinitiate.
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest,
       NodeReturnsInvalidReplicaSetConfigWhenInitiatingViaANodeThatCannotBecomePrimary) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();

    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Starting uninitialized, show that we can perform the initiate behavior.
    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"
                                                                             << "arbiterOnly"
                                                                             << true)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "node2:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "is not electable under the new configuration with");
    ASSERT_FALSE(getExternalState()->threadsStarted());
}

TEST_F(ReplCoordTest,
       InitiateShouldSucceedWithAValidConfigEvenIfItHasFailedWithAnInvalidConfigPreviously) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(opCtx.get(), BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Having failed to initiate once, show that we can now initiate.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest,
       NodeReturnsInvalidReplicaSetConfigWhenInitiatingWithAConfigTheNodeIsAbsentFrom) {
    BSONObjBuilder result;
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node4"))),
                                               &result));
}

void doReplSetInitiate(ReplicationCoordinatorImpl* replCoord, Status* status) {
    BSONObjBuilder garbage;
    auto client = getGlobalServiceContext()->makeClient("rsi");
    auto opCtx = client->makeOperationContext();
    *status =
        replCoord->processReplSetInitiate(opCtx.get(),
                                          BSON("_id"
                                               << "mySet"
                                               << "version" << 1 << "members"
                                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                        << "node1:12345")
                                                             << BSON("_id" << 1 << "host"
                                                                           << "node2:54321"))),
                                          &garbage);
}

TEST_F(ReplCoordTest, NodeReturnsNodeNotFoundWhenQuorumCheckFailsWhileInitiating) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(1);
    hbArgs.setConfigTerm(0);
    hbArgs.setCheckEmpty();
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);
    hbArgs.setTerm(0);
    hbArgs.setHeartbeatVersion(1);

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread([&] { doReplSetInitiate(getReplCoord(), &status); });
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(hbArgs.toBSON(), noi->getRequest().cmdObj);
    getNet()->scheduleResponse(noi,
                               startDate + Milliseconds(10),
                               RemoteCommandResponse(ErrorCodes::NoSuchKey, "No response"));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateSucceedsWhenQuorumCheckPasses) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(1);
    hbArgs.setConfigTerm(0);
    hbArgs.setCheckEmpty();
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);
    hbArgs.setTerm(0);
    hbArgs.setHeartbeatVersion(1);

    auto appliedTS = Timestamp(3, 3);
    replCoordSetMyLastAppliedOpTime(OpTime(appliedTS, 1), Date_t() + Seconds(100));

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread([&] { doReplSetInitiate(getReplCoord(), &status); });
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setConfigVersion(0);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    getNet()->scheduleResponse(
        noi, startDate + Milliseconds(10), RemoteCommandResponse(hbResp.toBSON(), Milliseconds(8)));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_OK(status);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

    ASSERT_EQUALS(getStorageInterface()->getInitialDataTimestamp(), appliedTS);
}

TEST_F(ReplCoordTest,
       NodeReturnsInvalidReplicaSetConfigWhenInitiatingWithAConfigWithAMismatchedSetName) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "wrongSet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenInitiatingWithAnEmptyConfig) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(opCtx.get(), BSONObj(), &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "'ReplSetConfig._id' is missing but a required field");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeReturnsInvalidReplicaSetConfigWhenInitiatingWithoutAn_idField) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status = getReplCoord()->processReplSetInitiate(
        opCtx.get(),
        BSON("version" << 1 << "members"
                       << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                << "node1:12345"))),
        &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "'ReplSetConfig._id' is missing but a required field");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeReturnsInvalidReplicaSetConfigWhenInitiatingWithAConfigVersionNotEqualToOne) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    auto status =
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 2 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "have version 1, but found 2");
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlag) {
    init("");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_EQUALS(
        ErrorCodes::NoReplicationEnabled,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeReturnsOutOfDiskSpaceWhenInitiateCannotWriteConfigToDisk) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    getExternalState()->setStoreLocalConfigDocumentStatus(
        Status(ErrorCodes::OutOfDiskSpace, "The test set this"));
    ASSERT_EQUALS(
        ErrorCodes::OutOfDiskSpace,
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeReturnsNoReplicationEnabledWhenCheckReplEnabledForCommandWhileNotRunningWithRepl) {
    // pass in settings to avoid having a replSet
    ReplSettings settings;
    init(settings);
    start();

    // check status NoReplicationEnabled and empty result
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(
    ReplCoordTest,
    NodeReturnsNoReplicationEnabledAndInfoConfigsvrWhenCheckReplEnabledForCommandWhileConfigsvr) {
    ReplSettings settings;
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    init(settings);
    start();

    // check status NoReplicationEnabled and result mentions configsrv
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
    ASSERT_EQUALS(result.obj()["info"].String(), "configsvr");
    serverGlobalParams.clusterRole = ClusterRole::None;
}

TEST_F(
    ReplCoordTest,
    NodeReturnsNotYetInitializedAndInfoNeedToInitiatedWhenCheckReplEnabledForCommandWithoutConfig) {
    start();

    // check status NotYetInitialized and result mentions rs.initiate
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NotYetInitialized);
    ASSERT_TRUE(result.obj()["info"].String().find("rs.initiate") != std::string::npos);
}

TEST_F(ReplCoordTest, NodeReturnsOkWhenCheckReplEnabledForCommandAfterReceivingAConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    // check status OK and result is empty
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, Status::OK());
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, NodeReturnsImmediatelyWhenAwaitReplicationIsRanAgainstAStandaloneNode) {
    init("");
    auto opCtx = makeOperationContext();

    OpTimeWithTermOne time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 2;

    // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
    // awaitReplication will always work.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, NodeReturnsNotPrimaryWhenRunningAwaitReplicationAgainstASecondaryNode) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();

    OpTimeWithTermOne time(100, 1);

    // Waiting for 0 nodes always works
    WriteConcernOptions writeConcern(
        0, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoWaiting);

    // Node should fail to awaitReplication when not primary.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern);
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, statusAndDur.status);
}

TEST_F(ReplCoordTest, NodeReturnsOkWhenRunningAwaitReplicationAgainstPrimaryWithWTermOne) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time(100, 1);

    // Waiting for 0 nodes always works
    WriteConcernOptions writeConcern(
        0, WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoWaiting);

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto opCtx = makeOperationContext();

    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest,
       NodeReturnsWriteConcernFailedUntilASufficientNumberOfNodesHaveTheWriteDurable) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 2);
    OpTimeWithTermOne time2(100, 3);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;

    auto opCtx = makeOperationContext();
    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.w = 2;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    // Applied is not durable and will not satisfy WriteConcern with SyncMode JOURNAL.
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time2
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.w = 3;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 3, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, NodeReturnsWriteConcernFailedUntilASufficientNumberOfNodesHaveTheWrite) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 2);
    OpTimeWithTermOne time2(100, 3);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;

    auto opCtx = makeOperationContext();


    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.w = 2;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time2
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.w = 3;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest,
       NodeReturnsUnknownReplWriteConcernWhenAwaitReplicationReceivesAnInvalidWriteConcernMode) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node0")
                                          << BSON("_id" << 1 << "host"
                                                        << "node1")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3")
                                          << BSON("_id" << 4 << "host"
                                                        << "node4"))),
                       HostAndPort("node0"));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    // Test invalid write concern
    WriteConcernOptions invalidWriteConcern;
    invalidWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    invalidWriteConcern.w = "fakemode";

    auto opCtx = makeOperationContext();
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, invalidWriteConcern);
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern, statusAndDur.status);
}

TEST_F(
    ReplCoordTest,
    NodeReturnsWriteConcernFailedUntilASufficientSetOfNodesHaveTheWriteAndTheWriteIsInACommittedSnapshot) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "node0"
                                      << "tags"
                                      << BSON("dc"
                                              << "NA"
                                              << "rack"
                                              << "rackNA1"))
                           << BSON("_id" << 1 << "host"
                                         << "node1"
                                         << "tags"
                                         << BSON("dc"
                                                 << "NA"
                                                 << "rack"
                                                 << "rackNA2"))
                           << BSON("_id" << 2 << "host"
                                         << "node2"
                                         << "tags"
                                         << BSON("dc"
                                                 << "NA"
                                                 << "rack"
                                                 << "rackNA3"))
                           << BSON("_id" << 3 << "host"
                                         << "node3"
                                         << "tags"
                                         << BSON("dc"
                                                 << "EU"
                                                 << "rack"
                                                 << "rackEU1"))
                           << BSON("_id" << 4 << "host"
                                         << "node4"
                                         << "tags"
                                         << BSON("dc"
                                                 << "EU"
                                                 << "rack"
                                                 << "rackEU2")))
             << "settings"
             << BSON("getLastErrorModes" << BSON("multiDC" << BSON("dc" << 2) << "multiDCAndRack"
                                                           << BSON("dc" << 2 << "rack" << 3)))),
        HostAndPort("node0"));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time1(Timestamp(100, 2), 1);
    OpTime time2(Timestamp(100, 3), 1);

    // Set up valid write concerns for the rest of the test
    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.w = WriteConcernOptions::kMajority;
    majorityWriteConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;

    WriteConcernOptions multiDCWriteConcern;
    multiDCWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiDCWriteConcern.w = "multiDC";

    WriteConcernOptions multiRackWriteConcern;
    multiRackWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiRackWriteConcern.w = "multiDCAndRack";

    auto opCtx = makeOperationContext();
    // Nothing satisfied
    getStorageInterface()->allDurableTimestamp = time1.getTimestamp();
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // Majority satisfied but not either custom mode
    getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1).transitional_ignore();
    getReplCoord()->setLastDurableOptime_forTest(2, 1, time1).transitional_ignore();
    getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1).transitional_ignore();
    getReplCoord()->setLastDurableOptime_forTest(2, 2, time1).transitional_ignore();

    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // All modes satisfied
    getReplCoord()->setLastAppliedOptime_forTest(2, 3, time1).transitional_ignore();
    getReplCoord()->setLastDurableOptime_forTest(2, 3, time1).transitional_ignore();

    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, multiRackWriteConcern);
    ASSERT_OK(statusAndDur.status);

    // multiDC satisfied but not majority or multiRack
    getStorageInterface()->allDurableTimestamp = time2.getTimestamp();
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    getReplCoord()->setLastAppliedOptime_forTest(2, 3, time2).transitional_ignore();
    getReplCoord()->setLastDurableOptime_forTest(2, 3, time2).transitional_ignore();

    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
}

/**
 * Used to wait for replication in a separate thread without blocking execution of the test.
 * To use, set the optime and write concern to be passed to awaitReplication and then call
 * start(), which will spawn a thread that calls awaitReplication.  No calls may be made
 * on the ReplicationAwaiter instance between calling start and getResult().  After returning
 * from getResult(), you can call reset() to allow the awaiter to be reused for another
 * awaitReplication call.
 */
class ReplicationAwaiter {
public:
    ReplicationAwaiter(ReplicationCoordinatorImpl* replCoord, ServiceContext* service)
        : _replCoord(replCoord),
          _service(service),
          _client(service->makeClient("replAwaiter")),
          _opCtx(_client->makeOperationContext()),
          _finished(false),
          _result(ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0))) {}

    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    void setOpTime(const OpTime& ot) {
        _optime = ot;
    }

    void setWriteConcern(WriteConcernOptions wc) {
        _writeConcern = wc;
    }

    // may block
    ReplicationCoordinator::StatusAndDuration getResult() {
        _thread.join();
        ASSERT(_finished);
        return _result;
    }

    void start() {
        ASSERT(!_finished);
        _thread = stdx::thread([this] { _awaitReplication(); });
    }

    void reset() {
        ASSERT(_finished);
        _finished = false;
        _result = ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
    }

private:
    void _awaitReplication() {
        _result = _replCoord->awaitReplication(_opCtx.get(), _optime, _writeConcern);
        _finished = true;
    }

    ReplicationCoordinatorImpl* _replCoord;
    ServiceContext* _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    bool _finished;
    OpTime _optime;
    WriteConcernOptions _writeConcern;
    ReplicationCoordinator::StatusAndDuration _result;
    stdx::thread _thread;
};

TEST_F(ReplCoordTest, NodeReturnsOkWhenAWriteConcernWithNoTimeoutHasBeenSatisfied) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 2;

    // 2 nodes waiting for time1
    awaiter.setOpTime(time1);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.start();
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 3 nodes waiting for time2
    writeConcern.w = 3;
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}


TEST_F(ReplCoordTest, NodeCalculatesDefaultWriteConcernOnStartupExistingLocalConfigMajority) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest().get());
}


TEST_F(ReplCoordTest,
       NodeCalculatesDefaultWriteConcernOnStartupExistingLocalConfigNoMajorityDueToArbiter) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest());
    ASSERT_FALSE(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest().get());
}


TEST_F(ReplCoordTest, NodeCalculatesDefaultWriteConcernOnStartupNewConfigMajority) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
    auto opCtx = makeOperationContext();

    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(1);
    hbArgs.setConfigTerm(0);
    hbArgs.setCheckEmpty();
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);
    hbArgs.setTerm(0);
    hbArgs.setHeartbeatVersion(1);

    auto appliedTS = Timestamp(3, 3);
    replCoordSetMyLastAppliedOpTime(OpTime(appliedTS, 1), Date_t() + Seconds(100));

    stdx::thread prsiThread([&] {
        BSONObjBuilder result1;
        ASSERT_OK(
            getReplCoord()->processReplSetInitiate(opCtx.get(),
                                                   BSON("_id"
                                                        << "mySet"
                                                        << "version" << 1 << "members"
                                                        << BSON_ARRAY(BSON("host"
                                                                           << "node1:12345"
                                                                           << "_id" << 0)
                                                                      << BSON("host"
                                                                              << "node2:12345"
                                                                              << "_id" << 1))),
                                                   &result1));
    });
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setConfigVersion(0);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    getNet()->scheduleResponse(
        noi, startDate + Milliseconds(10), RemoteCommandResponse(hbResp.toBSON(), Milliseconds(8)));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

    ASSERT_EQUALS(getStorageInterface()->getInitialDataTimestamp(), appliedTS);

    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest().get());
}


TEST_F(ReplCoordTest, NodeCalculatesDefaultWriteConcernOnStartupNewConfigNoMajorityDueToArbiter) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
    auto opCtx = makeOperationContext();

    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(1);
    hbArgs.setConfigTerm(0);
    hbArgs.setCheckEmpty();
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);
    hbArgs.setTerm(0);
    hbArgs.setHeartbeatVersion(1);

    auto appliedTS = Timestamp(3, 3);
    replCoordSetMyLastAppliedOpTime(OpTime(appliedTS, 1), Date_t() + Seconds(100));

    stdx::thread prsiThread([&] {
        BSONObjBuilder result1;
        ASSERT_OK(getReplCoord()->processReplSetInitiate(
            opCtx.get(),
            BSON("_id"
                 << "mySet"
                 << "version" << 1 << "members"
                 << BSON_ARRAY(BSON("host"
                                    << "node1:12345"
                                    << "_id" << 0)
                               << BSON("host"
                                       << "node2:12345"
                                       << "_id" << 1 << "arbiterOnly" << true))),
            &result1));
    });
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setConfigVersion(0);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    getNet()->scheduleResponse(
        noi, startDate + Milliseconds(10), RemoteCommandResponse(hbResp.toBSON(), Milliseconds(8)));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

    ASSERT_EQUALS(getStorageInterface()->getInitialDataTimestamp(), appliedTS);

    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    ASSERT(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest());
    ASSERT_FALSE(rwcDefaults.getImplicitDefaultWriteConcernMajority_forTest().get());
}


TEST_F(ReplCoordTest, NodeReturnsWriteConcernFailedWhenAWriteConcernTimesOutBeforeBeingSatisified) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wDeadline = getNet()->now() + Milliseconds(50);
    writeConcern.w = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    {
        NetworkInterfaceMock::InNetworkGuard inNet(getNet());
        getNet()->runUntil(writeConcern.wDeadline);
        ASSERT_EQUALS(writeConcern.wDeadline, getNet()->now());
    }
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsShutDownInProgressWhenANodeShutsDownPriorToSatisfyingAWriteConcern) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1));
    {
        auto opCtx = makeOperationContext();
        shutdown(opCtx.get());
    }
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, SupportTaggedWriteConcern) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node1:12345"
                                                     << "tags"
                                                     << BSON("donor"
                                                             << "node"))
                                          << BSON("_id" << 1 << "host"
                                                        << "node2:12345"
                                                        << "tags"
                                                        << BSON("recipient"
                                                                << "two"))
                                          << BSON("_id" << 2 << "host"
                                                        << "node3:12345"
                                                        << "tags"
                                                        << BSON("recipient"
                                                                << "three")))),
                       HostAndPort("node1", 12345));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    auto writeConcern =
        uassertStatusOK(WriteConcernOptions::parse(BSON("w" << BSON("recipient" << 2))));

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    // start nodes in a lagged state
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1));

    // catch them up to time2
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time2));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));

    ReplicationCoordinator::StatusAndDuration sad = awaiter.getResult();
    ASSERT_OK(sad.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, NodeReturnsNotPrimaryWhenSteppingDownBeforeSatisfyingAWriteConcern) {
    // Test that a thread blocked in awaitReplication will be woken up and return PrimarySteppedDown
    // (a NotPrimaryError) if the node steps down while it is waiting.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    const auto opCtx = makeOperationContext();
    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1));
    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsInterruptedWhenAnOpWaitingForWriteConcernToBeSatisfiedIsInterrupted) {
    // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node1")
                                          << BSON("_id" << 1 << "host"
                                                        << "node2")
                                          << BSON("_id" << 2 << "host"
                                                        << "node3"))),
                       HostAndPort("node1"));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 2;


    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1));

    killOperation(awaiter.getOperationContext());
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::Interrupted, statusAndDur.status);
    awaiter.reset();
}

class StepDownTest : public ReplCoordTest {
protected:
    struct SharedClientAndOperation {
        static SharedClientAndOperation make(ServiceContext* serviceContext) {
            SharedClientAndOperation result;
            result.client = serviceContext->makeClient("StepDownThread");
            result.opCtx = result.client->makeOperationContext();
            return result;
        }
        std::shared_ptr<Client> client;
        std::shared_ptr<OperationContext> opCtx;
    };

    std::pair<SharedClientAndOperation, stdx::future<boost::optional<Status>>> stepDown_nonBlocking(
        bool force, Milliseconds waitTime, Milliseconds stepDownTime) {
        using PromisedClientAndOperation = stdx::promise<SharedClientAndOperation>;
        auto task = stdx::packaged_task<boost::optional<Status>(PromisedClientAndOperation)>(
            [=](PromisedClientAndOperation operationPromise) -> boost::optional<Status> {
                auto result = SharedClientAndOperation::make(getServiceContext());
                operationPromise.set_value(result);
                try {
                    getReplCoord()->stepDown(result.opCtx.get(), force, waitTime, stepDownTime);
                    return Status::OK();
                } catch (const DBException& e) {
                    return e.toStatus();
                }
            });
        auto result = task.get_future();
        PromisedClientAndOperation operationPromise;
        auto operationFuture = operationPromise.get_future();
        stdx::thread(std::move(task), std::move(operationPromise)).detach();

        getReplCoord()->waitForStepDownAttempt_forTest();

        return std::make_pair(operationFuture.get(), std::move(result));
    }

    // Makes it so enough secondaries are caught up that a stepdown command can succeed.
    void catchUpSecondaries(const OpTime& desiredOpTime, Date_t desiredWallTime = Date_t()) {
        auto heartbeatInterval = getReplCoord()->getConfigHeartbeatInterval();
        if (desiredWallTime == Date_t() && !desiredOpTime.isNull()) {
            desiredWallTime = Date_t() + Seconds(desiredOpTime.getSecs());
        }

        enterNetwork();
        getNet()->runUntil(getNet()->now() + heartbeatInterval);
        NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        RemoteCommandRequest request = noi->getRequest();
        LOGV2(21497,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(hbArgs.getSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(hbArgs.getConfigVersion());
            hbResp.setAppliedOpTimeAndWallTime({desiredOpTime, desiredWallTime});
            hbResp.setDurableOpTimeAndWallTime({desiredOpTime, desiredWallTime});
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj);
            getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
        }
        while (getNet()->hasReadyRequests()) {
            auto noi = getNet()->getNextReadyRequest();
            LOGV2(21498,
                  "Blackholing network request {noi_getRequest_cmdObj}",
                  "noi_getRequest_cmdObj"_attr = noi->getRequest().cmdObj);
            getNet()->blackHole(noi);
        }

        getNet()->runReadyNetworkOperations();
        exitNetwork();
    }

    OID rid2;
    OID rid3;

private:
    virtual void setUp() {
        ReplCoordTest::setUp();
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(BSON("_id"
                                << "mySet"
                                << "version" << 1 << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "test1:1234")
                                              << BSON("_id" << 1 << "host"
                                                            << "test2:1234")
                                              << BSON("_id" << 2 << "host"
                                                            << "test3:1234"))),
                           HostAndPort("test1", 1234));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    }
};


TEST_F(ReplCoordTest, NodeReturnsBadValueWhenUpdateTermIsRunAgainstANonReplNode) {
    init(ReplSettings());
    ASSERT_TRUE(ReplicationCoordinator::modeNone == getReplCoord()->getReplicationMode());
    auto opCtx = makeOperationContext();

    ASSERT_EQUALS(ErrorCodes::BadValue, getReplCoord()->updateTerm(opCtx.get(), 0).code());
}

TEST_F(ReplCoordTest, UpdatePositionArgsAdvancesWallTimes) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test1", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    const auto repl = getReplCoord();
    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 1);

    repl->setMyLastAppliedOpTimeAndWallTime({opTime2, Date_t() + Seconds(1)});
    repl->setMyLastAppliedOpTimeAndWallTime({opTime2, Date_t() + Seconds(2)});

    // Secondaries not caught up yet.
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, opTime1, Date_t()));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1, Date_t()));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    // Catch up the secondaries using only replSetUpdatePosition.
    UpdatePositionArgs updatePositionArgs;

    Date_t memberOneAppliedWallTime = Date_t() + Seconds(3);
    Date_t memberOneDurableWallTime = Date_t() + Seconds(4);
    Date_t memberTwoAppliedWallTime = Date_t() + Seconds(5);
    Date_t memberTwoDurableWallTime = Date_t() + Seconds(6);

    ASSERT_OK(updatePositionArgs.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName << memberOneAppliedWallTime
                    << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName << memberOneDurableWallTime)
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName << memberTwoAppliedWallTime
                       << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << memberTwoDurableWallTime)))));

    ASSERT_OK(repl->processReplSetUpdatePosition(updatePositionArgs));

    // Make sure wall times are propagated through processReplSetUpdatePosition
    auto memberDataVector = repl->getMemberData();
    for (auto member : memberDataVector) {
        if (member.getMemberId() == MemberId(1)) {
            ASSERT_EQ(member.getLastAppliedWallTime(), memberOneAppliedWallTime);
            ASSERT_EQ(member.getLastDurableWallTime(), memberOneDurableWallTime);
        } else if (member.getMemberId() == MemberId(2)) {
            ASSERT_EQ(member.getLastAppliedWallTime(), memberTwoAppliedWallTime);
            ASSERT_EQ(member.getLastDurableWallTime(), memberTwoDurableWallTime);
        }
    }
}

TEST_F(ReplCoordTest, ElectionIdTracksTermInPV1) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion" << 1),
                       HostAndPort("test1", 1234));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // No election has taken place yet.
    {
        auto term = getTopoCoord().getTerm();
        auto electionId = getTopoCoord().getElectionId();

        ASSERT_EQUALS(0, term);
        ASSERT_FALSE(electionId.isSet());
    }

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Check that the electionId is set properly after the election.
    {
        auto term = getTopoCoord().getTerm();
        auto electionId = getTopoCoord().getElectionId();

        ASSERT_EQUALS(1, term);
        ASSERT_EQUALS(OID::fromTerm(term), electionId);
    }

    const auto opCtx = makeOperationContext();
    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));

    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1ElectionWithoutExitingDrainMode(
        getReplCoord()->getElectionTimeout_forTest(), opCtx.get());

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Check that the electionId is again properly set after the new election.
    {
        auto term = getTopoCoord().getTerm();
        auto electionId = getTopoCoord().getElectionId();

        ASSERT_EQUALS(2, term);
        ASSERT_EQUALS(OID::fromTerm(term), electionId);
    }
}

TEST_F(ReplCoordTest, NodeChangesTermAndStepsDownWhenAndOnlyWhenUpdateTermSuppliesAHigherTerm) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion" << 1),
                       HostAndPort("test1", 1234));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1Election();
    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // lower term, no change
    ASSERT_OK(getReplCoord()->updateTerm(opCtx.get(), 0));
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // same term, no change
    ASSERT_OK(getReplCoord()->updateTerm(opCtx.get(), 1));
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Check that the numStepDownsCausedByHigherTerm election metric is 0 to start with.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(0,
                  ReplicationMetrics::get(svcCtx).getNumStepDownsCausedByHigherTerm_forTesting());

    // higher term, step down and change term
    executor::TaskExecutor::CallbackHandle cbHandle;
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(opCtx.get(), 2).code());
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Check that the numStepDownsCausedByHigherTerm election metric has been incremented.
    ASSERT_EQUALS(1,
                  ReplicationMetrics::get(svcCtx).getNumStepDownsCausedByHigherTerm_forTesting());
}

TEST_F(ReplCoordTest, ConcurrentStepDownShouldNotSignalTheSameFinishEventMoreThanOnce) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion" << 1),
                       HostAndPort("test1", 1234));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1Election();

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    auto replExec = getReplExec();

    // Prevent _stepDownFinish() from running and becoming secondary by blocking in this
    // exclusive task.
    const auto opCtx = makeOperationContext();
    boost::optional<ReplicationStateTransitionLockGuard> transitionGuard;
    transitionGuard.emplace(opCtx.get(), MODE_X);

    TopologyCoordinator::UpdateTermResult termUpdated2;
    auto updateTermEvh2 = getReplCoord()->updateTerm_forTest(2, &termUpdated2);
    ASSERT(termUpdated2 == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
    ASSERT(updateTermEvh2.isValid());

    // Check that the numStepDownsCausedByHigherTerm election metric has been incremented.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(1,
                  ReplicationMetrics::get(svcCtx).getNumStepDownsCausedByHigherTerm_forTesting());

    TopologyCoordinator::UpdateTermResult termUpdated3;
    auto updateTermEvh3 = getReplCoord()->updateTerm_forTest(3, &termUpdated3);
    ASSERT(termUpdated3 == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
    // Although term 3 can trigger stepdown, a stepdown has already been scheduled,
    // so no other stepdown can be scheduled again. Term 3 will be remembered and
    // installed once stepdown finishes.
    ASSERT(!updateTermEvh3.isValid());

    // Check that the numStepDownsCausedByHigherTerm election metric has not been incremented a
    // second time.
    ASSERT_EQUALS(1,
                  ReplicationMetrics::get(svcCtx).getNumStepDownsCausedByHigherTerm_forTesting());

    // Unblock the tasks for updateTerm and _stepDownFinish.
    transitionGuard.reset();

    // Wait stepdown to finish and term 3 to be installed.
    replExec->waitForEvent(updateTermEvh2);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
}

TEST_F(ReplCoordTest, DrainCompletionMidStepDown) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion" << 1),
                       HostAndPort("test1", 1234));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    const auto opCtx = makeOperationContext();
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(
        getReplCoord()->getElectionTimeout_forTest(), opCtx.get());

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now update term to trigger a stepdown.
    TopologyCoordinator::UpdateTermResult termUpdated;
    auto updateTermEvh = getReplCoord()->updateTerm_forTest(2, &termUpdated);
    ASSERT(updateTermEvh.isValid());
    ASSERT(termUpdated == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);

    // Set 'firstOpTimeOfMyTerm' to have term 1, so that the node will see that the noop entry has
    // the correct term at the end of signalDrainComplete.
    getExternalState()->setFirstOpTimeOfMyTerm(OpTime(Timestamp(100, 1), 1));
    // Now signal that replication applier is finished draining its buffer.
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());

    // Now wait for stepdown to complete
    getReplExec()->waitForEvent(updateTermEvh);

    // By now, the node should have left drain mode.
    ASSERT(ReplicationCoordinator::ApplierState::Draining != getReplCoord()->getApplierState());

    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    // ASSERT_EQUALS(2, getReplCoord()->getTerm()); // SERVER-28290
}

TEST_F(StepDownTest, StepDownCanCompleteBasedOnReplSetUpdatePositionAlone) {
    const auto repl = getReplCoord();

    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 1);

    replCoordSetMyLastAppliedOpTime(opTime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(opTime2, Date_t() + Seconds(100));

    // Secondaries not caught up yet.
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, opTime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // The node has not been able to step down yet.
    ASSERT_TRUE(repl->getMemberState().primary());

    // Catch up one of the secondaries using only replSetUpdatePosition.
    UpdatePositionArgs updatePositionArgs;

    ASSERT_OK(updatePositionArgs.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime1.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(opTime1.asOpTime().getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << opTime1.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(opTime1.asOpTime().getSecs()))))));

    ASSERT_OK(repl->processReplSetUpdatePosition(updatePositionArgs));

    // Verify that stepDown completes successfully.
    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownFailureRestoresDrainState) {
    const auto repl = getReplCoord();

    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 1);

    replCoordSetMyLastAppliedOpTime(opTime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(opTime2, Date_t() + Seconds(100));

    // Secondaries not caught up yet.
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, opTime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1));

    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    const auto opCtx = makeOperationContext();
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTimeoutWhen, opCtx.get());
    ASSERT_TRUE(repl->getMemberState().primary());
    ASSERT(repl->getApplierState() == ReplicationCoordinator::ApplierState::Draining);

    {
        // We can't take writes yet since we're still in drain mode.
        Lock::GlobalLock lock(opCtx.get(), MODE_IX);
        ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase(opCtx.get(), "admin"));
    }

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // Interrupt the ongoing stepdown command so that the stepdown attempt will fail.
    {
        stdx::lock_guard<Client> lk(*result.first.client.get());
        result.first.opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // Ensure that the stepdown command failed.
    auto stepDownStatus = *result.second.get();
    ASSERT_NOT_OK(stepDownStatus);
    // Which code is returned is racy.
    ASSERT(stepDownStatus == ErrorCodes::PrimarySteppedDown ||
           stepDownStatus == ErrorCodes::Interrupted);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT(repl->getApplierState() == ReplicationCoordinator::ApplierState::Draining);

    // Ensure that the failed stepdown attempt didn't make us able to take writes since we're still
    // in drain mode.
    {
        Lock::GlobalLock lock(opCtx.get(), MODE_IX);
        ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase(opCtx.get(), "admin"));
    }

    // Now complete drain mode and ensure that we become capable of taking writes.
    signalDrainComplete(opCtx.get());
    ASSERT(repl->getApplierState() == ReplicationCoordinator::ApplierState::Stopped);

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(opCtx.get(), "admin"));
}

class StepDownTestWithUnelectableNode : public StepDownTest {
private:
    void setUp() override {
        ReplCoordTest::setUp();
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(BSON("_id"
                                << "mySet"
                                << "version" << 1 << "protocolVersion" << 1 << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "test1:1234")
                                              << BSON("_id" << 1 << "host"
                                                            << "test2:1234"
                                                            << "priority" << 0)
                                              << BSON("_id" << 2 << "host"
                                                            << "test3:1234"))),
                           HostAndPort("test1", 1234));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    }
};

TEST_F(StepDownTestWithUnelectableNode,
       UpdatePositionDuringStepDownWakesUpStepDownWaiterMoreThanOnce) {
    const auto repl = getReplCoord();

    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 1);

    replCoordSetMyLastAppliedOpTime(opTime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(opTime2, Date_t() + Seconds(100));

    // No secondaries are caught up yet.
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, opTime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // The node has not been able to step down yet.
    ASSERT_TRUE(repl->getMemberState().primary());

    // Use replSetUpdatePosition to catch up the first secondary, which is not electable.
    // This will yield a majority at the primary's opTime, so the waiter will be woken up,
    // but stepDown will not be able to complete.
    UpdatePositionArgs catchupFirstSecondary;

    ASSERT_OK(catchupFirstSecondary.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime1.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(opTime1.asOpTime().getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << opTime1.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(opTime1.asOpTime().getSecs()))))));

    ASSERT_OK(repl->processReplSetUpdatePosition(catchupFirstSecondary));

    // The primary has still not been able to finish stepping down.
    ASSERT_TRUE(repl->getMemberState().primary());

    // Now catch up the other secondary. This will wake up the waiter again, but this time
    // there is an electable node, so stepDown will complete.
    UpdatePositionArgs catchupOtherSecondary;

    ASSERT_OK(catchupOtherSecondary.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(opTime2.asOpTime().getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << repl->getConfigVersion() << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(opTime2.asOpTime().getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(opTime2.asOpTime().getSecs()))))));

    ASSERT_OK(repl->processReplSetUpdatePosition(catchupOtherSecondary));

    // Verify that stepDown completes successfully.
    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, NodeReturnsNotWritablePrimaryWhenAskedToStepDownAsANonPrimaryNode) {
    const auto opCtx = makeOperationContext();

    OpTimeWithTermOne optime1(100, 1);
    // All nodes are caught up
    replCoordSetMyLastAppliedOpTime(optime1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime1, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    ASSERT_THROWS_CODE(
        getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(0)),
        AssertionException,
        ErrorCodes::NotWritablePrimary);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeReturnsExceededTimeLimitWhenStepDownFailsToObtainTheGlobalLockWithinTheAllottedTime) {
    OpTimeWithTermOne optime1(100, 1);

    // Set up this test so that all nodes are caught up. This is necessary to exclude the false
    // positive case where stepDown returns "ExceededTimeLimit", but not because it could not
    // acquire the lock, but because it could not satisfy all stepdown conditions on time.
    replCoordSetMyLastAppliedOpTime(optime1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime1, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    const auto opCtx = makeOperationContext();

    // Make sure stepDown cannot grab the RSTL in mode X. We need to use a different
    // locker to test this, or otherwise stepDown will be granted the lock automatically.
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);
    ASSERT_TRUE(opCtx->lockState()->isRSTLExclusive());
    auto locker =
        getClient()->swapLockState(std::make_unique<LockerImpl>(opCtx->getServiceContext()));

    ASSERT_THROWS_CODE(
        getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000)),
        AssertionException,
        ErrorCodes::LockTimeout);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    ASSERT_TRUE(locker->isRSTLExclusive());
    ASSERT_FALSE(opCtx->lockState()->isRSTLLocked());

    getClient()->swapLockState(std::move(locker));
}

/* Step Down Test for a 5-node replica set */
class StepDownTestFiveNode : public StepDownTest {
protected:
    /*
     * Simulate a round of heartbeat requests from the primary by manually setting
     * the heartbeat response messages from each node. 'numNodesCaughtUp' will
     * determine how many nodes return an optime that is up to date with the
     * primary's optime. Sets electability of all caught up nodes to 'caughtUpAreElectable'
     */
    void simulateHeartbeatResponses(OpTime optimePrimary,
                                    OpTime optimeLagged,
                                    int numNodesCaughtUp,
                                    Date_t wallTimePrimary = Date_t(),
                                    Date_t wallTimeLagged = Date_t()) {
        int hbNum = 1;
        if (wallTimePrimary == Date_t()) {
            wallTimePrimary = Date_t() + Seconds(optimePrimary.getSecs());
        }
        if (wallTimeLagged == Date_t()) {
            wallTimeLagged = Date_t() + Seconds(optimeLagged.getSecs());
        }
        while (getNet()->hasReadyRequests()) {
            NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
            RemoteCommandRequest request = noi->getRequest();

            // Only process heartbeat requests.
            ASSERT_EQ(request.cmdObj.firstElement().fieldNameStringData().toString(),
                      "replSetHeartbeat");

            ReplSetHeartbeatArgsV1 hbArgs;
            ASSERT_OK(hbArgs.initialize(request.cmdObj));

            LOGV2(21499,
                  "{request_target} processing {request_cmdObj}",
                  "request_target"_attr = request.target.toString(),
                  "request_cmdObj"_attr = request.cmdObj);

            // Catch up 'numNodesCaughtUp' nodes out of 5.
            OpTime optimeResponse = (hbNum <= numNodesCaughtUp) ? optimePrimary : optimeLagged;
            Date_t wallTimeResponse =
                (hbNum <= numNodesCaughtUp) ? wallTimePrimary : wallTimeLagged;

            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(hbArgs.getSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(hbArgs.getConfigVersion());
            hbResp.setDurableOpTimeAndWallTime({optimeResponse, wallTimeResponse});
            hbResp.setAppliedOpTimeAndWallTime({optimeResponse, wallTimeResponse});
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj);
            getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
            hbNum += 1;
        }
    }

private:
    virtual void setUp() {
        ReplCoordTest::setUp();
        init("mySet/test1:1234,test2:1234,test3:1234,test4:1234,test5:1234");

        assertStartSuccess(BSON("_id"
                                << "mySet"
                                << "version" << 1 << "members"
                                << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                         << "test1:1234")
                                              << BSON("_id" << 1 << "host"
                                                            << "test2:1234")
                                              << BSON("_id" << 2 << "host"
                                                            << "test3:1234")
                                              << BSON("_id" << 3 << "host"
                                                            << "test4:1234")
                                              << BSON("_id" << 4 << "host"
                                                            << "test5:1234"))),
                           HostAndPort("test1", 1234));
        ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    }
};

TEST_F(StepDownTestFiveNode,
       NodeReturnsExceededTimeLimitWhenStepDownIsRunAndNoCaughtUpMajorityExists) {
    OpTime optimeLagged(Timestamp(100, 1), 1);
    OpTime optimePrimary(Timestamp(100, 2), 1);

    // All nodes are caught up
    replCoordSetMyLastAppliedOpTime(optimePrimary, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optimePrimary, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 3, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 4, optimeLagged));

    simulateSuccessfulV1Election();

    enterNetwork();
    getNet()->runUntil(getNet()->now() + Seconds(2));
    ASSERT(getNet()->hasReadyRequests());

    // Make sure less than a majority are caught up (i.e. 2 out of 5) We catch up one secondary
    // since the primary counts as one towards majority
    int numNodesCaughtUp = 1;
    simulateHeartbeatResponses(optimePrimary, optimeLagged, numNodesCaughtUp);
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    const auto opCtx = makeOperationContext();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT_THROWS_CODE(
        getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000)),
        AssertionException,
        ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(
    StepDownTestFiveNode,
    NodeTransitionsToSecondaryImmediatelyWhenStepDownIsRunAndAnUpToDateMajorityWithElectableNodeExists) {
    OpTime optimeLagged(Timestamp(100, 1), 1);
    OpTime optimePrimary(Timestamp(100, 2), 1);

    // All nodes are caught up
    replCoordSetMyLastAppliedOpTime(optimePrimary, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optimePrimary, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 3, optimeLagged));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 4, optimeLagged));

    simulateSuccessfulV1Election();

    enterNetwork();
    getNet()->runUntil(getNet()->now() + Seconds(2));
    ASSERT(getNet()->hasReadyRequests());

    // Make sure a majority are caught up (i.e. 3 out of 5). We catch up two secondaries since
    // the primary counts as one towards majority
    int numNodesCaughtUp = 2;
    simulateHeartbeatResponses(optimePrimary, optimeLagged, numNodesCaughtUp);
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    const auto opCtx = makeOperationContext();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000));
    enterNetwork();  // So we can safely inspect the topology coordinator
    ASSERT_EQUALS(getNet()->now() + Seconds(1), getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

// This test checks if a primary is chosen even if there are two simultaneous elections
// happening because of election timeout and step-down timeout in a single node replica set.
TEST_F(ReplCoordTest, SingleNodeReplSetStepDownTimeoutAndElectionTimeoutExpiresAtTheSameTime) {
    init();

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "protocolVersion" << 1 << "settings"
                            << BSON("electionTimeoutMillis" << 1000)),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    getExternalState()->setElectionTimeoutOffsetLimitFraction(0);
    runSingleNodeElection(opCtx.get());

    // Stepdown command with "force=true" resets the election timer to election timeout (10 seconds
    // later) and allows the node to resume primary after stepdown timeout (also 10 seconds).
    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    getNet()->enterNetwork();
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Now run time forward and make sure that the node becomes primary again when stepdown timeout
    // and election timeout occurs at the same time.
    Date_t stepdownUntil = getNet()->now() + Seconds(1);
    getNet()->runUntil(stepdownUntil);
    ASSERT_EQUALS(stepdownUntil, getNet()->now());
    ASSERT_TRUE(getTopoCoord().getMemberState().primary());
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

// We cancel and rescheduled election timeout after syncing a batch from our sync source, if our
// sync source reports a primaryIndex in its OplogQueryMetadata. The batch's
// ReplSetMetadata.isPrimary field is ignored.
TEST_F(ReplCoordTest, CancelElectionTimeoutIfSyncSourceKnowsThePrimary) {
    init();

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234"))
                            << "protocolVersion" << 1 << "settings"
                            << BSON("electionTimeoutMillis" << 1000)),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    getExternalState()->setElectionTimeoutOffsetLimitFraction(0);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    DataReplicatorExternalStateImpl state(getReplCoord(), getExternalState());
    auto electionTimeout = getReplCoord()->getElectionTimeout_forTest();

    // Advance clock time so we know whether the election timeout was rescheduled.
    auto net = this->getNet();
    net->enterNetwork();
    net->advanceTime(net->now() + Milliseconds(10));
    net->exitNetwork();

    ReplSetMetadata rsMeta(0, OpTimeAndWallTime(), OpTime(), 1, 0, OID(), 1, false /* isPrimary */);

    // If currentPrimaryIndex is -1, don't reschedule.
    state.processMetadata(
        rsMeta,
        OplogQueryMetadata(OpTimeAndWallTime(), OpTime(), 1, -1 /* currentPrimaryIndex */, 1, ""));

    ASSERT_EQUALS(getReplCoord()->getElectionTimeout_forTest(), electionTimeout);

    // If currentPrimaryIndex is NOT -1, reschedule.
    state.processMetadata(
        rsMeta,
        OplogQueryMetadata(OpTimeAndWallTime(), OpTime(), 1, 1 /* currentPrimaryIndex */, 1, ""));

    // Since we advanced the clock, the new election timeout is after the old one.
    ASSERT_GREATER_THAN(getReplCoord()->getElectionTimeout_forTest(), electionTimeout);
}

TEST_F(ReplCoordTest, ShouldChangeSyncSource) {
    init();

    const OID replicaSetId = OID::gen();
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
                             << BSON("replicaSetId" << replicaSetId));
    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    getTopoCoord().updateConfig(config, 1, getNet()->now());

    OplogQueryMetadata opMetaData(
        OpTimeAndWallTime(), OpTime(Timestamp(1, 1), 1), 1, 0 /* currentPrimaryIndex */, 1, "");
    ReplSetMetadata rsMeta(
        0, OpTimeAndWallTime(), OpTime(), 1, 0, replicaSetId, 1, true /* isPrimary */);

    ASSERT_EQ(getReplCoord()->shouldChangeSyncSource(
                  HostAndPort("node1", 12345), rsMeta, opMetaData, OpTime(), OpTime()),
              ChangeSyncSourceAction::kContinueSyncing);

    ASSERT_EQ(getReplCoord()->shouldChangeSyncSource(
                  HostAndPort("node4", 12345), rsMeta, opMetaData, OpTime(), OpTime()),
              ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch);
}

TEST_F(ReplCoordTest, ServerlessNodeShouldChangeSyncSourceAfterSplit) {
    // Test that ReplicationCoordinator will correctly stop enqueuing messages from the donor and
    // change sync source when started in serverless mode and the replicaSetId changes.

    ReplSettings settings;
    settings.setServerlessMode();
    init(settings);

    const OID replicaSetId = OID::gen();
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
                             << BSON("replicaSetId" << replicaSetId));

    assertStartSuccess(configObj, HostAndPort("node1", 12345));
    ReplSetConfig config = assertMakeRSConfig(configObj);

    getTopoCoord().updateConfig(config, 1, getNet()->now());

    OplogQueryMetadata opMetaData(
        OpTimeAndWallTime(), OpTime(Timestamp(1, 1), 1), 1, 0 /* currentPrimaryIndex */, 1, "");
    ReplSetMetadata rsMeta(
        0, OpTimeAndWallTime(), OpTime(), 1, 0, replicaSetId, 1, true /* isPrimary */);

    // Continue syncing when the node is in the replica set and the replicaSetId is the same.
    ASSERT_EQ(getReplCoord()->shouldChangeSyncSource(
                  HostAndPort("node1", 12345), rsMeta, opMetaData, OpTime(), OpTime()),
              ChangeSyncSourceAction::kContinueSyncing);

    // Enqueue final message but stop syncing when the replicaSetId is the same but the node is not
    // in the replica set anymore.
    ASSERT_EQ(getReplCoord()->shouldChangeSyncSource(
                  HostAndPort("node4", 12345), rsMeta, opMetaData, OpTime(), OpTime()),
              ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch);

    // Discard messages and stop syncing when the node is not in the replica set anymore and the
    // replicaSetId has changed. This case occurs after a successfull shard split.
    ReplSetMetadata rsMeta2(
        0, OpTimeAndWallTime(), OpTime(), 1, 0, OID::gen(), 1, false /* isPrimary */);
    ASSERT_EQ(getReplCoord()->shouldChangeSyncSource(
                  HostAndPort("node4", 12345), rsMeta2, opMetaData, OpTime(), OpTime()),
              ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent);
}

TEST_F(ReplCoordTest, SingleNodeReplSetUnfreeze) {
    init();

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "protocolVersion" << 1 << "settings"
                            << BSON("electionTimeoutMillis" << 10000)),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    getExternalState()->setElectionTimeoutOffsetLimitFraction(0);

    // Become Secondary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Freeze the node for 20 seconds.
    BSONObjBuilder resultObj;
    Date_t freezeUntil = getNet()->now() + Seconds(15);
    ASSERT_OK(getReplCoord()->processReplSetFreeze(20, &resultObj));
    getNet()->enterNetwork();

    // Now run time forward and unfreeze the node after 15 seconds.
    getNet()->runUntil(freezeUntil);
    ASSERT_EQUALS(freezeUntil, getNet()->now());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_OK(getReplCoord()->processReplSetFreeze(0, &resultObj));
    getNet()->enterNetwork();

    // Wait for single node election to happen.
    Date_t waitUntil = freezeUntil + Seconds(1);
    getNet()->runUntil(waitUntil);
    ASSERT_EQUALS(waitUntil, getNet()->now());
    ASSERT_TRUE(getTopoCoord().getMemberState().primary());
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Check that the numFreezeTimeoutsCalled and the numFreezeTimeoutsSuccessful election metrics
    // have been incremented, and that none of the metrics that track the number of elections called
    // or successful for other reasons has been incremented.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsSuccessful_forTesting());
    ASSERT_EQUALS(0,
                  ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsSuccessful_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsSuccessful_forTesting());
}

TEST_F(ReplCoordTest, NodeBecomesPrimaryAgainWhenStepDownTimeoutExpiresInASingleNodeSet) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

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

    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    getNet()->enterNetwork();  // Must do this before inspecting the topocoord
    Date_t stepdownUntil = getNet()->now() + Seconds(1);
    ASSERT_EQUALS(stepdownUntil, getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Now run time forward and make sure that the node becomes primary again when the stepdown
    // period ends.
    getNet()->runUntil(stepdownUntil);
    ASSERT_EQUALS(stepdownUntil, getNet()->now());
    ASSERT_TRUE(getTopoCoord().getMemberState().primary());
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Check that the numFreezeTimeoutsCalled and the numFreezeTimeoutsSuccessful election metrics
    // have been incremented, and that none of the metrics that track the number of elections called
    // or successful for other reasons has been incremented. When a stepdown timeout expires in a
    // single node replica set, an election is called for the same reason as is used when a freeze
    // timeout expires.
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsCalled_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsCalled_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumStepUpCmdsSuccessful_forTesting());
    ASSERT_EQUALS(0,
                  ReplicationMetrics::get(svcCtx).getNumPriorityTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(0, ReplicationMetrics::get(svcCtx).getNumCatchUpTakeoversSuccessful_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumElectionTimeoutsSuccessful_forTesting());
    ASSERT_EQUALS(1, ReplicationMetrics::get(svcCtx).getNumFreezeTimeoutsSuccessful_forTesting());
}

TEST_F(
    ReplCoordTest,
    NodeGoesIntoRecoveryAgainWhenStepDownTimeoutExpiresInASingleNodeSetAndWeAreInMaintenanceMode) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    const auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    getNet()->enterNetwork();  // Must do this before inspecting the topocoord
    Date_t stepdownUntil = getNet()->now() + Seconds(1);
    ASSERT_EQUALS(stepdownUntil, getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Go into maintenance mode.
    ASSERT_EQUALS(0, getTopoCoord().getMaintenanceCount());
    ASSERT_FALSE(getReplCoord()->getMaintenanceMode());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_EQUALS(1, getTopoCoord().getMaintenanceCount());
    ASSERT_TRUE(getReplCoord()->getMaintenanceMode());

    // Now run time forward and make sure that the node goes into RECOVERING again when the stepdown
    // period ends.
    getNet()->runUntil(stepdownUntil);
    ASSERT_EQUALS(stepdownUntil, getNet()->now());
    ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING), getTopoCoord().getMemberState());
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING), getReplCoord()->getMemberState());
}

TEST_F(StepDownTest,
       NodeReturnsExceededTimeLimitWhenNoSecondaryIsCaughtUpWithinStepDownsSecondaryCatchUpPeriod) {
    OpTimeWithTermOne optime1(100, 1);
    OpTimeWithTermOne optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    const auto opCtx = makeOperationContext();

    // Try to stepDown but time out because no secondaries are caught up.
    ASSERT_THROWS_CODE(
        getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000)),
        AssertionException,
        ErrorCodes::ExceededTimeLimit);
    ASSERT_TRUE(repl->getMemberState().primary());

    // Now use "force" to force it to step down even though no one is caught up
    getNet()->enterNetwork();
    const Date_t startDate = getNet()->now();
    while (startDate + Milliseconds(1000) < getNet()->now()) {
        while (getNet()->hasReadyRequests()) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
        getNet()->runUntil(startDate + Milliseconds(1000));
    }
    getNet()->exitNetwork();
    ASSERT_TRUE(repl->getMemberState().primary());
    repl->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeTransitionsToSecondaryWhenASecondaryCatchesUpAfterTheFirstRoundOfHeartbeats) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    catchUpSecondaries(optime2, Date_t() + Seconds(optime2.getSecs()));

    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeTransitionsToSecondaryWhenASecondaryCatchesUpDuringStepDownsSecondaryCatchupPeriod) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // Advance the clock by two seconds to allow for a round of heartbeats to be sent. The
    // secondary will not appear to be caught up.
    enterNetwork();
    getNet()->runUntil(getNet()->now() + Milliseconds(2000));
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    LOGV2(21500,
          "HB1: {request_target} processing {request_cmdObj}",
          "request_target"_attr = request.target.toString(),
          "request_cmdObj"_attr = request.cmdObj);
    ReplSetHeartbeatArgsV1 hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj);
        getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
    }
    while (getNet()->hasReadyRequests()) {
        getNet()->blackHole(getNet()->getNextReadyRequest());
    }
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    catchUpSecondaries(optime2);

    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest, NodeReturnsInterruptedWhenInterruptedDuringStepDown) {
    OpTimeWithTermOne optime1(100, 1);
    OpTimeWithTermOne optime2(100, 2);
    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(repl->getMemberState().primary());

    // stepDown where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));
    killOperation(result.first.opCtx.get());
    ASSERT_EQUALS(ErrorCodes::Interrupted, *result.second.get());
    ASSERT_TRUE(repl->getMemberState().primary());
}

TEST_F(StepDownTest, OnlyOneStepDownCmdIsAllowedAtATime) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now while the first stepdown request is waiting for secondaries to catch up, attempt
    // another stepdown request and ensure it fails.
    const auto opCtx = makeOperationContext();
    ASSERT_THROWS_CODE(getReplCoord()->stepDown(opCtx.get(), false, Seconds(10), Seconds(60)),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    // Now ensure that the original stepdown command can still succeed.
    catchUpSecondaries(optime2);

    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

// Test that if a stepdown command is blocked waiting for secondaries to catch up when an
// unconditional stepdown happens, the stepdown command fails.
TEST_F(StepDownTest, UnconditionalStepDownFailsStepDownCommand) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Start a stepdown command that needs to wait for secondaries to catch up.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now while the first stepdown request is waiting for secondaries to catch up, force an
    // unconditional stepdown.
    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(opCtx.get(), 2));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Ensure that the stepdown command failed.
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, *result.second.get());
}

// Test that if a stepdown command is blocked waiting for secondaries to catch up when an
// unconditional stepdown happens, and then is interrupted, we step back up.
TEST_F(StepDownTest, InterruptingStepDownCommandRestoresWriteAvailability) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Start a stepdown command that needs to wait for secondaries to catch up.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // We should not indicate that we are a writable primary, nor that we are secondary.
    auto opCtx = makeOperationContext();
    auto response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());

    // Interrupt the ongoing stepdown command.
    {
        stdx::lock_guard<Client> lk(*result.first.client.get());
        result.first.opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // Ensure that the stepdown command failed.
    ASSERT_EQUALS(*result.second.get(), ErrorCodes::Interrupted);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // We should now report that we are a writable primary.
    response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_TRUE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());

    // This is the important check, that we stepped back up when aborting the stepdown command
    // attempt.
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_TRUE(getReplCoord()->canAcceptWritesForDatabase(opCtx.get(), "admin"));
}

// Test that if a stepdown command is blocked waiting for secondaries to catch up when an
// unconditional stepdown happens, and then is interrupted, we stay stepped down, even though
// normally if we were just interrupted we would step back up.
TEST_F(StepDownTest, InterruptingAfterUnconditionalStepdownDoesNotRestoreWriteAvailability) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    replCoordSetMyLastAppliedOpTime(optime2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime2, Date_t() + Seconds(100));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Start a stepdown command that needs to wait for secondaries to catch up.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // We should not indicate that we are a writable primary, nor that we are secondary.
    auto opCtx = makeOperationContext();
    auto response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ;
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());

    // Interrupt the ongoing stepdown command.
    {
        stdx::lock_guard<Client> lk(*result.first.client.get());
        result.first.opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // Now while the first stepdown request is waiting for secondaries to catch up, force an
    // unconditional stepdown.
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(opCtx.get(), 2));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Ensure that the stepdown command failed.
    auto stepDownStatus = *result.second.get();
    ASSERT_NOT_OK(stepDownStatus);
    // Which code is returned is racy.
    ASSERT(stepDownStatus == ErrorCodes::PrimarySteppedDown ||
           stepDownStatus == ErrorCodes::Interrupted);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // We should still be indicating that we are not a writable primary.
    response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(response->isWritablePrimary());

    // This is the important check, that we didn't accidentally step back up when aborting the
    // stepdown command attempt.
    Lock::GlobalLock lock(opCtx.get(), MODE_IX);
    ASSERT_FALSE(getReplCoord()->canAcceptWritesForDatabase(opCtx.get(), "admin"));
}

TEST_F(ReplCoordTest, GetReplicationModeNone) {
    init();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeReturnsModeReplSetInResponseToGetReplicationModeWhenRunningWithTheReplSetFlag) {
    // modeReplSet if the set name was supplied.
    ReplSettings settings;
    settings.setReplSetString("mySet/node1:12345");
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
}

TEST_F(ReplCoordTest, NodeIncludesOtherMembersProgressInUpdatePositionCommand) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234")
                                          << BSON("_id" << 3 << "host"
                                                        << "test4:1234"))),
                       HostAndPort("test1", 1234));
    OpTime optime1({2, 1}, 1);
    OpTime optime2({100, 1}, 1);
    OpTime optime3({100, 2}, 1);
    replCoordSetMyLastAppliedOpTime(optime1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime1, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime2));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime3));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(1, 2, optime3));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 3, optime3));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(1, 3, optime1));

    // Check that the proper BSON is generated for the replSetUpdatePositionCommand
    BSONObj cmd = unittest::assertGet(getReplCoord()->prepareReplSetUpdatePositionCommand());

    ASSERT_EQUALS(3, cmd.nFields());
    ASSERT_EQUALS(UpdatePositionArgs::kCommandFieldName, cmd.firstElement().fieldNameStringData());

    std::set<long long> memberIds;
    BSONForEach(entryElement, cmd[UpdatePositionArgs::kUpdateArrayFieldName].Obj()) {
        OpTime durableOpTime;
        OpTime appliedOpTime;
        BSONObj entry = entryElement.Obj();
        long long memberId = entry[UpdatePositionArgs::kMemberIdFieldName].Number();
        memberIds.insert(memberId);
        if (memberId == 0) {
            LOGV2(21501, "{_0}", "_0"_attr = 0);
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime1, appliedOpTime);
            ASSERT_EQUALS(optime1, durableOpTime);
        } else if (memberId == 1) {
            LOGV2(21502, "{_1}", "_1"_attr = 1);
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime2, appliedOpTime);
            ASSERT_EQUALS(OpTime(), durableOpTime);
        } else if (memberId == 2) {
            LOGV2(21503, "{_2}", "_2"_attr = 2);
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime3, appliedOpTime);
            ASSERT_EQUALS(optime3, durableOpTime);
        } else {
            LOGV2(21504, "{_3}", "_3"_attr = 3);
            ASSERT_EQUALS(3, memberId);
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime3, appliedOpTime);
            ASSERT_EQUALS(optime1, durableOpTime);
        }
    }
    ASSERT_EQUALS(4U, memberIds.size());  // Make sure we saw all 4 nodes
}

TEST_F(ReplCoordTest,
       NodeReturnsOperationFailedWhenSettingMaintenanceModeFalseWhenItHasNotBeenSetTrue) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    const auto opCtx = makeOperationContext();

    // Can't unset maintenance mode if it was never set to begin with.
    Status status = getReplCoord()->setMaintenanceMode(opCtx.get(), false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest,
       ReportRollbackWhileInBothRollbackAndMaintenanceModeAndRecoveryAfterFinishingRollback) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    const auto opCtx = makeOperationContext();

    // valid set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

    // If we go into rollback while in maintenance mode, our state changes to RS_ROLLBACK.
    ASSERT_OK(getReplCoord()->setFollowerModeRollback(opCtx.get()));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // When we go back to SECONDARY, we still observe RECOVERING because of maintenance mode.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
}

TEST_F(ReplCoordTest, AllowAsManyUnsetMaintenanceModesAsThereHaveBeenSetMaintenanceModes) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    const auto opCtx = makeOperationContext();

    // Can set multiple times
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));

    // Need to unset the number of times you set.
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), false));
    Status status = getReplCoord()->setMaintenanceMode(opCtx.get(), false);
    // third one fails b/c we only set two times.
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    // Unsetting maintenance mode changes our state to secondary if maintenance mode was
    // the only thing keeping us out of it.
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, SettingAndUnsettingMaintenanceModeShouldNotAffectRollbackState) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    const auto opCtx = makeOperationContext();
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

    // From rollback, entering and exiting maintenance mode doesn't change perceived
    // state.
    ASSERT_OK(getReplCoord()->setFollowerModeRollback(opCtx.get()));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // Rollback is sticky even if entered while in maintenance mode.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
    ASSERT_OK(getReplCoord()->setFollowerModeRollback(opCtx.get()));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, DoNotAllowMaintenanceModeWhilePrimary) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    // Can't modify maintenance mode when PRIMARY
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    Status status = getReplCoord()->setMaintenanceMode(opCtx.get(), true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down from primary.
    getReplCoord()->updateTerm(opCtx.get(), getReplCoord()->getTerm() + 1).transitional_ignore();
    ASSERT_OK(
        getReplCoord()->waitForMemberState(opCtx.get(), MemberState::RS_SECONDARY, Seconds(1)));

    status = getReplCoord()->setMaintenanceMode(opCtx.get(), false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(opCtx.get(), false));
}

TEST_F(ReplCoordTest, DoNotAllowSettingMaintenanceModeWhileConductingAnElection) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    // TODO this election shouldn't have to happen.
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();


    // Step down from primary.
    getReplCoord()->updateTerm(opCtx.get(), getReplCoord()->getTerm() + 1).transitional_ignore();
    getReplCoord()
        ->waitForMemberState(opCtx.get(), MemberState::RS_SECONDARY, Milliseconds(10 * 1000))
        .transitional_ignore();

    // Can't modify maintenance mode when running for election (before and after dry run).
    ASSERT_EQUALS(TopologyCoordinator::Role::kFollower, getTopoCoord().getRole());
    auto net = this->getNet();
    net->enterNetwork();
    auto when = getReplCoord()->getElectionTimeout_forTest();
    while (net->now() < when) {
        net->runUntil(when);
        if (!net->hasReadyRequests()) {
            continue;
        }
        net->blackHole(net->getNextReadyRequest());
    }
    ASSERT_EQUALS(when, net->now());
    net->exitNetwork();
    ASSERT_EQUALS(TopologyCoordinator::Role::kCandidate, getTopoCoord().getRole());
    Status status = getReplCoord()->setMaintenanceMode(opCtx.get(), false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(opCtx.get(), true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    simulateSuccessfulDryRun();
    ASSERT_EQUALS(TopologyCoordinator::Role::kCandidate, getTopoCoord().getRole());
    status = getReplCoord()->setMaintenanceMode(opCtx.get(), false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(opCtx.get(), true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);

    // This cancels the actual election.
    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the vote requester.
    ASSERT_EQUALS(ErrorCodes::ElectionInProgress,
                  getReplCoord()->setFollowerModeRollback(opCtx.get()));
}

TEST_F(ReplCoordTest,
       NodeReturnsACompleteListOfNodesWeKnowHaveTheWriteDurablyInResponseToGetHostsWrittenTo) {
    HostAndPort myHost("node1:12345");
    HostAndPort client1Host("node2:12345");
    HostAndPort client2Host("node3:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString())
                                          << BSON("_id" << 1 << "host" << client1Host.toString())
                                          << BSON("_id" << 2 << "host" << client2Host.toString()))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 1, time1));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2, true);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(myHost, caughtUpHosts[0]);

    // Ensure updating applied does not affect the results for getHostsWritten durably.
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2, true);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(myHost, caughtUpHosts[0]);

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time2));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2, true);
    ASSERT_EQUALS(2U, caughtUpHosts.size());
    if (myHost == caughtUpHosts[0]) {
        ASSERT_EQUALS(client2Host, caughtUpHosts[1]);
    } else {
        ASSERT_EQUALS(client2Host, caughtUpHosts[0]);
        ASSERT_EQUALS(myHost, caughtUpHosts[1]);
    }
}

TEST_F(ReplCoordTest,
       NodeReturnsACompleteListOfNodesWeKnowHaveTheWriteInResponseToGetHostsWrittenTo) {
    HostAndPort myHost("node1:12345");
    HostAndPort client1Host("node2:12345");
    HostAndPort client2Host("node3:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString())
                                          << BSON("_id" << 1 << "host" << client1Host.toString())
                                          << BSON("_id" << 2 << "host" << client2Host.toString()))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2, false);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(myHost, caughtUpHosts[0]);

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2, false);
    ASSERT_EQUALS(2U, caughtUpHosts.size());
    if (myHost == caughtUpHosts[0]) {
        ASSERT_EQUALS(client2Host, caughtUpHosts[1]);
    } else {
        ASSERT_EQUALS(client2Host, caughtUpHosts[0]);
        ASSERT_EQUALS(myHost, caughtUpHosts[1]);
    }
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsCurrentTopologyVersionOnTimeOut) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, {}, deadline);
        auto topologyVersion = response->getTopologyVersion();
        // Assert that on timeout, the returned HelloResponse contains the same TopologyVersion.
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedTopologyVersion.getCounter());
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedTopologyVersion.getProcessId());
    });

    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloThread.join();
    getNet()->exitNetwork();
}

TEST_F(ReplCoordTest,
       AwaitHelloResponseReturnsCurrentTopologyVersionOnRequestWithDifferentProcessId) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto topologyVersion = getTopoCoord().getTopologyVersion();

    // Get the HelloResponse for a request that contains a different process ID. This
    // should return immediately in all cases instead of waiting for a topology change.
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, topologyVersion.getProcessId());

    // Test receiving a TopologyVersion with a different process ID but the same counter.
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, topologyVersion.getCounter());
    ASSERT_EQUALS(topologyVersionWithDifferentProcessId.getCounter(), topologyVersion.getCounter());
    auto response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    auto responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), topologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), topologyVersion.getCounter());

    // Increment the counter of topologyVersionWithDifferentProcessId.
    topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, topologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(topologyVersionWithDifferentProcessId.getCounter(),
                        topologyVersion.getCounter());

    // Test receiving a TopologyVersion with a different process ID and a greater counter.
    response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), topologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), topologyVersion.getCounter());
}

TEST_F(ReplCoordTest,
       AwaitHelloResponseReturnsCurrentTopologyVersionOnRequestWithStaleTopologyVersion) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto staleTopologyVersion = getTopoCoord().getTopologyVersion();

    // Update the TopologyVersion in the TopologyCoordinator.
    getTopoCoord().incrementTopologyVersion();
    auto updatedTopologyVersion = getTopoCoord().getTopologyVersion();
    ASSERT_LESS_THAN(staleTopologyVersion.getCounter(), updatedTopologyVersion.getCounter());

    // Get the HelloResponse for a request that contains a stale TopologyVersion. This should
    // return immediately instead of blocking and waiting for a topology change.
    auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, staleTopologyVersion, deadline);
    auto responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), updatedTopologyVersion.getCounter());
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), updatedTopologyVersion.getProcessId());
}

TEST_F(ReplCoordTest, AwaitHelloResponseFailsOnRequestWithFutureTopologyVersion) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto topologyVersion = getTopoCoord().getTopologyVersion();
    auto futureTopologyVersion =
        TopologyVersion(topologyVersion.getProcessId(), topologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(futureTopologyVersion.getCounter(), topologyVersion.getCounter());

    // We should fail immediately if trying to build an HelloResponse for a request with a
    // greater TopologyVersion.
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, futureTopologyVersion, deadline),
        AssertionException,
        31382);
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnStepDown) {
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
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
        auto expectedProcessId = currentTopologyVersion.getProcessId();
        // A topology change should increment the TopologyVersion counter.
        auto expectedCounter = currentTopologyVersion.getCounter() + 1;

        const auto responseAfterDisablingWrites =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);
        const auto topologyVersionAfterDisablingWrites =
            responseAfterDisablingWrites->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterDisablingWrites->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterDisablingWrites->getProcessId(), expectedProcessId);
        // We expect the server to increment the TopologyVersion and respond to waiting hellos
        // once we disable writes on the node that is stepping down from primary. At this time,
        // the 'ismaster' response field will be false but the node will have yet to transition to
        // secondary.
        ASSERT_FALSE(responseAfterDisablingWrites->isWritablePrimary());
        ASSERT_FALSE(responseAfterDisablingWrites->isSecondary());
        ASSERT_EQUALS(responseAfterDisablingWrites->getPrimary().host(), "node1");

        // The server TopologyVersion will increment a second time once the old primary has
        // completed its transition to secondary. A hello request with
        // 'topologyVersionAfterDisablingWrites' should get a response immediately since that
        // TopologyVersion is now stale.
        expectedCounter = topologyVersionAfterDisablingWrites->getCounter() + 1;
        deadline = getNet()->now() + maxAwaitTime;
        const auto responseStepdownComplete = awaitHelloWithNewOpCtx(
            getReplCoord(), topologyVersionAfterDisablingWrites.get(), {}, deadline);
        const auto topologyVersionStepDownComplete = responseStepdownComplete->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionStepDownComplete->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionStepDownComplete->getProcessId(), expectedProcessId);
        ASSERT_FALSE(responseStepdownComplete->isWritablePrimary());
        ASSERT_TRUE(responseStepdownComplete->isSecondary());
        ASSERT_FALSE(responseStepdownComplete->hasPrimary());
    });

    // Ensure that awaitHelloResponse() is called before triggering a stepdown.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    // A topology change should cause the server to respond to the waiting HelloResponse.
    getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    getHelloThread.join();
}

TEST_F(ReplCoordTest, HelloReturnsErrorOnEnteringQuiesceMode) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto opCtx = makeOperationContext();
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    stdx::thread getHelloThread([&] {
        auto maxAwaitTime = Milliseconds(5000);
        auto deadline = getNet()->now() + maxAwaitTime;

        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline),
            AssertionException,
            ErrorCodes::ShutdownInProgress);
    });

    // Ensure that awaitHelloResponse() is called before entering quiesce mode.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    ASSERT(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));
    ASSERT_EQUALS(currentTopologyVersion.getCounter() + 1,
                  getTopoCoord().getTopologyVersion().getCounter());
    // Check that the cached topologyVersion counter was updated correctly.
    ASSERT_EQUALS(getTopoCoord().getTopologyVersion().getCounter(),
                  getReplCoord()->getTopologyVersion().getCounter());
    getHelloThread.join();
}

TEST_F(ReplCoordTest, HelloReturnsErrorOnEnteringQuiesceModeAfterWaitingTimesOut) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto opCtx = makeOperationContext();
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    AtomicWord<bool> helloReturned{false};
    stdx::thread getHelloThread([&] {
        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline),
            AssertionException,
            ErrorCodes::ShutdownInProgress);
        helloReturned.store(true);
    });

    auto failPoint = globalFailPointRegistry().find("hangAfterWaitingForTopologyChangeTimesOut");
    auto timesEnteredFailPoint = failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] { failPoint->setMode(FailPoint::off, 0); });

    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getNet()->exitNetwork();

    // Ensure that waiting for a topology change timed out before entering quiesce mode.
    failPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    ASSERT(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));
    failPoint->setMode(FailPoint::off, 0);

    // Advance the clock so that pauseWhileSet() will wake up.
    while (!helloReturned.load()) {
        getNet()->enterNetwork();
        getNet()->advanceTime(getNet()->now() + Milliseconds(100));
        getNet()->exitNetwork();
    }

    getHelloThread.join();
}

TEST_F(ReplCoordTest, AlwaysDecrementNumAwaitingTopologyChangesOnErrorMongoD) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto opCtx = makeOperationContext();
    ASSERT_EQUALS(0, HelloMetrics::get(opCtx.get())->getNumAwaitingTopologyChanges());

    auto hangFP = globalFailPointRegistry().find("hangAfterWaitingForTopologyChangeTimesOut");
    auto timesEnteredHangFP = hangFP->setMode(FailPoint::alwaysOn);

    // Use a novel error code to test this functionality.
    auto expectedErrorCode = 6208201;
    auto customErrorFP = globalFailPointRegistry().find("setCustomErrorInHelloResponseMongoD");
    customErrorFP->setMode(FailPoint::alwaysOn, 0, BSON("errorType" << expectedErrorCode));
    ON_BLOCK_EXIT([&] { customErrorFP->setMode(FailPoint::off, 0); });

    auto deadline = getNet()->now() + Milliseconds(5000);
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    AtomicWord<bool> helloReturned{false};
    stdx::thread getHelloThread([&] {
        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline),
            AssertionException,
            ErrorCodes::Error(expectedErrorCode));
        helloReturned.store(true);
    });

    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getNet()->exitNetwork();

    hangFP->waitForTimesEntered(timesEnteredHangFP + 1);

    // Observe that the counter has been incremented.
    ASSERT_EQUALS(1, HelloMetrics::get(opCtx.get())->getNumAwaitingTopologyChanges());

    hangFP->setMode(FailPoint::off, 0);

    // Advance the clock so that pauseWhileSet() will wake up.
    while (!helloReturned.load()) {
        getNet()->enterNetwork();
        getNet()->advanceTime(getNet()->now() + Milliseconds(100));
        getNet()->exitNetwork();
    }
    getHelloThread.join();
    ASSERT_TRUE(helloReturned.load());

    // Make sure we still decremented the counter.
    ASSERT_EQUALS(0, HelloMetrics::get(opCtx.get())->getNumAwaitingTopologyChanges());
}

TEST_F(ReplCoordTest, HelloReturnsErrorInQuiesceMode) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    ASSERT(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(1000)));
    ASSERT_EQUALS(currentTopologyVersion.getCounter() + 1,
                  getTopoCoord().getTopologyVersion().getCounter());
    // Check that the cached topologyVersion counter was updated correctly.
    ASSERT_EQUALS(getTopoCoord().getTopologyVersion().getCounter(),
                  getReplCoord()->getTopologyVersion().getCounter());

    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    // Stale topology version
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, currentTopologyVersion, deadline),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Current topology version
    currentTopologyVersion = getTopoCoord().getTopologyVersion();
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, currentTopologyVersion, deadline),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Different process ID
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter());
    ASSERT_THROWS_CODE(getReplCoord()->awaitHelloResponse(
                           opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // No topology version
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Check that status includes an extraErrorInfo class. Since we did not advance the clock, we
    // should still have the full quiesceTime as our remaining quiesceTime.
    try {
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, currentTopologyVersion, deadline);
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ShutdownInProgressQuiesceInfo>());
        ASSERT_EQ(ex.extraInfo<ShutdownInProgressQuiesceInfo>()->getRemainingQuiesceTimeMillis(),
                  1000);
    }
}

TEST_F(ReplCoordTest, QuiesceModeErrorsReturnAccurateRemainingQuiesceTime) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto totalQuiesceTime = Milliseconds(1000);
    ASSERT(getReplCoord()->enterQuiesceModeIfSecondary(totalQuiesceTime));
    ASSERT_EQUALS(currentTopologyVersion.getCounter() + 1,
                  getTopoCoord().getTopologyVersion().getCounter());
    // Check that the cached topologyVersion counter was updated correctly.
    ASSERT_EQUALS(getTopoCoord().getTopologyVersion().getCounter(),
                  getReplCoord()->getTopologyVersion().getCounter());

    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    auto halfwayThroughQuiesce = getNet()->now() + totalQuiesceTime / 2;

    getNet()->enterNetwork();
    // Advance the clock halfway to the quiesce deadline.
    getNet()->advanceTime(halfwayThroughQuiesce);
    getNet()->exitNetwork();

    // Check that status includes an extraErrorInfo class. Since we advanced the clock halfway to
    // the quiesce deadline, we should have half of the total quiesceTime left, 500 ms.
    try {
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, currentTopologyVersion, deadline);
    } catch (const DBException& ex) {
        ASSERT(ex.extraInfo());
        ASSERT(ex.extraInfo<ShutdownInProgressQuiesceInfo>());
        ASSERT_EQ(ex.extraInfo<ShutdownInProgressQuiesceInfo>()->getRemainingQuiesceTimeMillis(),
                  500);
    }
}


TEST_F(ReplCoordTest, DoNotEnterQuiesceModeInStatesOtherThanSecondary) {
    init();

    // Do not enter quiesce mode in state RS_STARTUP.
    ASSERT_TRUE(getReplCoord()->getMemberState().startup());
    ASSERT_FALSE(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));

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

    // Do not enter quiesce mode in state RS_STARTUP2.
    ASSERT_TRUE(getReplCoord()->getMemberState().startup2());
    ASSERT_FALSE(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    // Do not enter quiesce mode in state RS_PRIMARY.
    ASSERT_FALSE(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));
}

TEST_F(ReplCoordTest, HelloReturnsErrorInQuiesceModeWhenNodeIsRemoved) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 1)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->cancelAndRescheduleElectionTimeout();

    // Enter quiesce mode. Test that we increment the topology version.
    auto topologyVersionBeforeQuiesceMode = getTopoCoord().getTopologyVersion();
    ASSERT(getReplCoord()->enterQuiesceModeIfSecondary(Milliseconds(0)));
    auto topologyVersionAfterQuiesceMode = getTopoCoord().getTopologyVersion();
    ASSERT_EQUALS(topologyVersionBeforeQuiesceMode.getCounter() + 1,
                  topologyVersionAfterQuiesceMode.getCounter());

    // Remove the node.
    auto net = getNet();
    enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
    ReplSetHeartbeatResponse hbResp;
    auto removedFromConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "protocolVersion" << 1 << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host"
                                                     << "node2:12345"
                                                     << "_id" << 2))));
    hbResp.setConfig(removedFromConfig);
    hbResp.setConfigVersion(2);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
    net->runReadyNetworkOperations();
    exitNetwork();

    // Wait for the node to be removed. Test that we increment the topology version.
    ASSERT_OK(getReplCoord()->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_REMOVED, Seconds(1)));
    ASSERT_EQUALS(removedFromConfig.getConfigVersion(), getReplCoord()->getConfigVersion());
    auto topologyVersionAfterRemoved = getTopoCoord().getTopologyVersion();
    ASSERT_EQUALS(topologyVersionAfterQuiesceMode.getCounter() + 1,
                  topologyVersionAfterRemoved.getCounter());

    // Test hello requests.

    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    // Stale topology version
    ASSERT_THROWS_CODE(getReplCoord()->awaitHelloResponse(
                           opCtx.get(), {}, topologyVersionAfterQuiesceMode, deadline),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // Current topology version
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, topologyVersionAfterRemoved, deadline),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Different process ID
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, topologyVersionAfterRemoved.getProcessId());
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, topologyVersionAfterRemoved.getCounter());
    ASSERT_THROWS_CODE(getReplCoord()->awaitHelloResponse(
                           opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // No topology version
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none),
        AssertionException,
        ErrorCodes::ShutdownInProgress);
}

TEST_F(ReplCoordTest, AllHelloResponseFieldsRespectHorizon) {
    init();
    const auto primaryHostName = "node1:12345";
    const auto primaryHostNameHorizon = "horizon.com:15";
    const auto passiveHostName = "node2:12345";
    const auto passiveHostNameHorizon = "horizon.com:16";
    const auto arbiterHostName = "node3:12345";
    const auto arbiterHostNameHorizon = "horizon.com:17";
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("host" << primaryHostName << "_id" << 1 << "horizons"
                                       << BSON("horizon" << primaryHostNameHorizon))
                           << BSON("host" << passiveHostName << "_id" << 2 << "horizons"
                                          << BSON("horizon" << passiveHostNameHorizon) << "priority"
                                          << 0)
                           << BSON("host" << arbiterHostName << "_id" << 3 << "horizons"
                                          << BSON("horizon" << arbiterHostNameHorizon)
                                          << "arbiterOnly" << true))),
        HostAndPort(primaryHostName));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto opCtx = makeOperationContext();

    // When no horizon is specified, the hello response uses the default horizon.
    {
        HostAndPort primaryHostAndPort(primaryHostName);
        HostAndPort passiveHostAndPort(passiveHostName);
        HostAndPort arbiterHostAndPort(arbiterHostName);

        const auto response =
            getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(hosts[0], primaryHostAndPort);
        ASSERT_EQUALS(response->getPrimary(), primaryHostAndPort);
        ASSERT_EQUALS(response->getMe(), primaryHostAndPort);
        const auto passives = response->getPassives();
        ASSERT_EQUALS(passives[0], passiveHostAndPort);
        const auto arbiters = response->getArbiters();
        ASSERT_EQUALS(arbiters[0], arbiterHostAndPort);
    }

    // The hello response respects the requested horizon.
    {
        HostAndPort primaryHostAndPort(primaryHostNameHorizon);
        HostAndPort passiveHostAndPort(passiveHostNameHorizon);
        HostAndPort arbiterHostAndPort(arbiterHostNameHorizon);

        const std::string horizonSniName = "horizon.com";
        const auto horizon = SplitHorizon::Parameters(horizonSniName);
        const auto response =
            getReplCoord()->awaitHelloResponse(opCtx.get(), horizon, boost::none, boost::none);
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(hosts[0], primaryHostAndPort);
        ASSERT_EQUALS(response->getPrimary(), primaryHostAndPort);
        ASSERT_EQUALS(response->getMe(), primaryHostAndPort);
        const auto passives = response->getPassives();
        ASSERT_EQUALS(passives[0], passiveHostAndPort);
        const auto arbiters = response->getArbiters();
        ASSERT_EQUALS(arbiters[0], arbiterHostAndPort);
    }
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsErrorOnHorizonChange) {
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
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline),
            AssertionException,
            ErrorCodes::SplitHorizonChange);
    });

    // Ensure that the hello request is waiting before doing a reconfig.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    // Use force to bypass the oplog commitment check, which we're not worried about testing here.
    args.force = true;
    // Do a reconfig that changes the SplitHorizon and also adds a third node. This should respond
    // to all waiting hello requests with an error.
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 3 << "horizons"
                                                      << BSON("testhorizon"
                                                              << "test.monkey.example.com:24"))
                                           << BSON("_id" << 1 << "host"
                                                         << "node2:12345"
                                                         << "horizons"
                                                         << BSON("testhorizon"
                                                                 << "test.giraffe.example.com:25"))
                                           << BSON("_id"
                                                   << 2 << "host"
                                                   << "node3:12345"
                                                   << "horizons"
                                                   << BSON("testhorizon"
                                                           << "test.elephant.example.com:26"))));
    stdx::thread reconfigThread([&] {
        Status status(ErrorCodes::InternalError, "Not Set");
        status = getReplCoord()->processReplSetReconfig(opCtx.get(), args, &garbage);
        ASSERT_OK(status);
    });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    getHelloThread.join();
}

TEST_F(ReplCoordTest, NonAwaitableHelloReturnsNoConfigsOnNodeWithUninitializedConfig) {
    start();
    auto opCtx = makeOperationContext();

    const auto response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, {}, {});
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());
}

TEST_F(ReplCoordTest, AwaitableHelloOnNodeWithUninitializedConfig) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();

    auto maxAwaitTime = Milliseconds(5000);
    auto halfwayToDeadline = getNet()->now() + maxAwaitTime / 2;
    auto deadline = getNet()->now() + maxAwaitTime;

    AtomicWord<bool> isHelloReturned{false};
    stdx::thread awaitHelloTimeout([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, {}, deadline);
        isHelloReturned.store(true);
        auto responseTopologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(expectedTopologyVersion.getProcessId(),
                      responseTopologyVersion->getProcessId());
        ASSERT_EQUALS(expectedTopologyVersion.getCounter(), responseTopologyVersion->getCounter());
        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_FALSE(response->isSecondary());
        ASSERT_FALSE(response->isConfigSet());
    });

    getNet()->enterNetwork();
    getNet()->advanceTime(halfwayToDeadline);
    ASSERT_EQUALS(halfwayToDeadline, getNet()->now());
    ASSERT_FALSE(isHelloReturned.load());

    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    awaitHelloTimeout.join();
    ASSERT_TRUE(isHelloReturned.load());
    getNet()->exitNetwork();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    deadline = getNet()->now() + maxAwaitTime;
    stdx::thread awaitHelloInitiate([&] {
        const auto topologyVersion = getTopoCoord().getTopologyVersion();
        const auto response = awaitHelloWithNewOpCtx(getReplCoord(), topologyVersion, {}, deadline);
        auto responseTopologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion.getProcessId(), responseTopologyVersion->getProcessId());
        ASSERT_EQUALS(topologyVersion.getCounter() + 1, responseTopologyVersion->getCounter());
        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_FALSE(response->isSecondary());
        ASSERT_TRUE(response->isConfigSet());
    });

    // Ensure that awaitHelloResponse() is called before initiating.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    BSONObjBuilder result;
    auto status =
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result);
    ASSERT_OK(status);
    awaitHelloInitiate.join();
}

TEST_F(ReplCoordTest, AwaitableHelloOnNodeWithUninitializedConfigDifferentTopologyVersion) {
    start();
    auto opCtx = makeOperationContext();

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    const auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // A request with a future TopologyVersion should error.
    const auto futureTopologyVersion = TopologyVersion(currentTopologyVersion.getProcessId(),
                                                       currentTopologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(futureTopologyVersion.getCounter(), currentTopologyVersion.getCounter());
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, futureTopologyVersion, deadline),
        AssertionException,
        31382);

    // A request with a stale TopologyVersion should return immediately with the current server
    // TopologyVersion.
    const auto staleTopologyVersion = TopologyVersion(currentTopologyVersion.getProcessId(),
                                                      currentTopologyVersion.getCounter() - 1);
    ASSERT_LESS_THAN(staleTopologyVersion.getCounter(), currentTopologyVersion.getCounter());
    auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, staleTopologyVersion, deadline);
    auto responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());

    // A request with a different processId should return immediately with the server processId.
    const auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter());
    response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());

    // A request with a future TopologyVersion but different processId should still return
    // immediately.
    topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(topologyVersionWithDifferentProcessId.getCounter(),
                        currentTopologyVersion.getCounter());
    response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());
}

TEST_F(ReplCoordTest, AwaitableHelloOnNodeWithUninitializedConfigInvalidHorizon) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    const std::string horizonSniName = "horizon.com";
    const auto horizonParam = SplitHorizon::Parameters(horizonSniName);

    // Send a non-awaitable hello.
    const auto initialResponse = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, {}, {});
    ASSERT_FALSE(initialResponse->isWritablePrimary());
    ASSERT_FALSE(initialResponse->isSecondary());
    ASSERT_FALSE(initialResponse->isConfigSet());

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    stdx::thread awaitHelloInitiate([&] {
        const auto topologyVersion = getTopoCoord().getTopologyVersion();
        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), topologyVersion, horizonParam, deadline),
            AssertionException,
            ErrorCodes::SplitHorizonChange);
    });

    // Ensure that the hello request has started waiting before initiating.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Call replSetInitiate with no horizon configured. This should return an error to the hello
    // request that is currently waiting on a horizonParam that doesn't exit in the config.
    BSONObjBuilder result;
    auto status =
        getReplCoord()->processReplSetInitiate(opCtx.get(),
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result);
    ASSERT_OK(status);
    awaitHelloInitiate.join();
}

TEST_F(ReplCoordTest, AwaitableHelloOnNodeWithUninitializedConfigSpecifiedHorizon) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    const std::string horizonSniName = "horizon.com";
    const auto horizonParam = SplitHorizon::Parameters(horizonSniName);

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    const std::string horizonOneSniName = "horizon1.com";
    const auto horizonOne = SplitHorizon::Parameters(horizonOneSniName);
    const auto horizonOneView = HostAndPort("horizon1.com:12345");
    stdx::thread awaitHelloInitiate([&] {
        const auto topologyVersion = getTopoCoord().getTopologyVersion();
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), topologyVersion, horizonOne, deadline);
        auto responseTopologyVersion = response->getTopologyVersion();
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(hosts[0], horizonOneView);
        ASSERT_EQUALS(topologyVersion.getProcessId(), responseTopologyVersion->getProcessId());
        ASSERT_EQUALS(topologyVersion.getCounter() + 1, responseTopologyVersion->getCounter());
        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_FALSE(response->isSecondary());
        ASSERT_TRUE(response->isConfigSet());
    });

    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Call replSetInitiate with a horizon configured.
    BSONObjBuilder result;
    auto status = getReplCoord()->processReplSetInitiate(
        opCtx.get(),
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "node1:12345"
                                      << "horizons"
                                      << BSON("horizon1"
                                              << "horizon1.com:12345")))),
        &result);
    ASSERT_OK(status);
    awaitHelloInitiate.join();
}

TEST_F(ReplCoordTest, AwaitHelloUsesDefaultHorizonWhenRequestedHorizonNotFound) {
    init();
    const auto nodeOneHostName = "node1:12345";
    const auto nodeTwoHostName = "node2:12345";
    const std::string nodeOneSniName = "node1.old.com";
    const auto oldHorizonNodeOne = "node1.old.com:24";
    const auto oldHorizonNodeTwo = "node2.old.com:25";
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("host" << nodeOneHostName << "_id" << 1 << "horizons"
                                       << BSON("oldhorizon" << oldHorizonNodeOne))
                           << BSON("host" << nodeTwoHostName << "_id" << 2 << "horizons"
                                          << BSON("oldhorizon" << oldHorizonNodeTwo)))),
        HostAndPort(nodeOneHostName));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    auto opCtx = makeOperationContext();

    const auto oldHorizon = SplitHorizon::Parameters(nodeOneSniName);

    stdx::thread getHelloOldHorizonThread([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, oldHorizon, deadline);
        auto topologyVersion = response->getTopologyVersion();
        const auto hosts = response->getHosts();
        HostAndPort expectedNodeOneHorizonView(oldHorizonNodeOne);
        HostAndPort expectedNodeTwoHorizonView(oldHorizonNodeTwo);
        ASSERT_EQUALS(hosts[0], expectedNodeOneHorizonView);
        ASSERT_EQUALS(hosts[1], expectedNodeTwoHorizonView);
        ASSERT_EQUALS(response->getPrimary(), expectedNodeOneHorizonView);
        ASSERT_EQUALS(response->getMe(), expectedNodeOneHorizonView);
    });

    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloOldHorizonThread.join();
    getNet()->exitNetwork();
    replyToReceivedHeartbeatV1();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = true;
    // Do a reconfig that removes the configured horizon.
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 2 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(BSON("host" << nodeOneHostName << "_id" << 1)
                                           << BSON("host" << nodeTwoHostName << "_id" << 2)));
    stdx::thread reconfigThread([&] {
        Status status(ErrorCodes::InternalError, "Not Set");
        status = getReplCoord()->processReplSetReconfig(opCtx.get(), args, &garbage);
        ASSERT_OK(status);
    });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();

    stdx::thread getHelloDefaultHorizonThread([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        // Sending a hello request with a removed horizon should return the default horizon.
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, oldHorizon, deadline);
        auto topologyVersion = response->getTopologyVersion();
        const auto hosts = response->getHosts();
        HostAndPort expectedNodeOneHorizonView(nodeOneHostName);
        HostAndPort expectedNodeTwoHorizonView(nodeTwoHostName);
        ASSERT_EQUALS(hosts[0], expectedNodeOneHorizonView);
        ASSERT_EQUALS(hosts[1], expectedNodeTwoHorizonView);
        ASSERT_EQUALS(response->getPrimary(), expectedNodeOneHorizonView);
        ASSERT_EQUALS(response->getMe(), expectedNodeOneHorizonView);
    });

    deadline = getNet()->now() + maxAwaitTime;
    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloDefaultHorizonThread.join();
    getNet()->exitNetwork();
}

TEST_F(ReplCoordTest, AwaitHelloRespondsWithNewHorizon) {
    init();
    const auto nodeOneHostName = "node1:12345";
    const auto nodeTwoHostName = "node2:12345";
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host" << nodeOneHostName << "_id" << 1)
                                          << BSON("host" << nodeTwoHostName << "_id" << 2))),
                       HostAndPort(nodeOneHostName));

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    auto opCtx = makeOperationContext();

    // Define a new horizon to be configured later.
    const std::string newHorizonSniName = "newhorizon.com";
    const auto newHorizon = SplitHorizon::Parameters(newHorizonSniName);

    stdx::thread getHelloThread([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        // The hello response should use the default horizon since no horizon has been
        // configured.
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, newHorizon, deadline);
        const auto hosts = response->getHosts();
        HostAndPort expectedNodeOneHorizonView(nodeOneHostName);
        HostAndPort expectedNodeTwoHorizonView(nodeTwoHostName);
        ASSERT_EQUALS(hosts[0], expectedNodeOneHorizonView);
        ASSERT_EQUALS(hosts[1], expectedNodeTwoHorizonView);
        ASSERT_EQUALS(response->getPrimary(), expectedNodeOneHorizonView);
        ASSERT_EQUALS(response->getMe(), expectedNodeOneHorizonView);
    });

    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloThread.join();
    getNet()->exitNetwork();
    replyToReceivedHeartbeatV1();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = true;
    // Do a reconfig that adds a new horizon.
    const auto newHorizonNodeOne = "newhorizon.com:15";
    const auto newHorizonNodeTwo = "newhorizon.com:16";
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 2 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(
                                    BSON("host" << nodeOneHostName << "_id" << 1 << "horizons"
                                                << BSON("newhorizon" << newHorizonNodeOne))
                                    << BSON("host" << nodeTwoHostName << "_id" << 2 << "horizons"
                                                   << BSON("newhorizon" << newHorizonNodeTwo))));
    stdx::thread reconfigThread([&] {
        Status status(ErrorCodes::InternalError, "Not Set");
        status = getReplCoord()->processReplSetReconfig(opCtx.get(), args, &garbage);
        ASSERT_OK(status);
    });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();

    stdx::thread getHelloNewHorizonThread([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        // The hello response should now use the newly configured horizon.
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, newHorizon, deadline);
        const auto hosts = response->getHosts();
        HostAndPort expectedNodeOneHorizonView(newHorizonNodeOne);
        HostAndPort expectedNodeTwoHorizonView(newHorizonNodeTwo);
        ASSERT_EQUALS(hosts[0], expectedNodeOneHorizonView);
        ASSERT_EQUALS(hosts[1], expectedNodeTwoHorizonView);
        ASSERT_EQUALS(response->getPrimary(), expectedNodeOneHorizonView);
        ASSERT_EQUALS(response->getMe(), expectedNodeOneHorizonView);
    });

    deadline = getNet()->now() + maxAwaitTime;
    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloNewHorizonThread.join();
    getNet()->exitNetwork();
}

TEST_F(ReplCoordTest, HelloOnRemovedNode) {
    init();
    const auto nodeOneHostName = "node1:12345";
    const auto nodeTwoHostName = "node2:12345";
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host" << nodeOneHostName << "_id" << 1)
                                          << BSON("host" << nodeTwoHostName << "_id" << 2))),
                       HostAndPort(nodeOneHostName));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto net = getNet();
    enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort(nodeTwoHostName), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Receive a config that excludes node1 and with node2 having a configured horizon.
    ReplSetHeartbeatResponse hbResp;
    auto removedFromConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "protocolVersion" << 1 << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << nodeTwoHostName << "_id" << 2
                                                            << "horizons"
                                                            << BSON("horizon1"
                                                                    << "testhorizon.com:100")))));
    hbResp.setConfig(removedFromConfig);
    hbResp.setConfigVersion(2);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
    net->runReadyNetworkOperations();
    exitNetwork();

    // node1 no longer exists in the replica set config.
    ASSERT_OK(getReplCoord()->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_REMOVED, Seconds(1)));
    ASSERT_EQUALS(removedFromConfig.getConfigVersion(), getReplCoord()->getConfigVersion());

    const auto maxAwaitTime = Milliseconds(5000);
    auto deadline = net->now() + maxAwaitTime;
    auto opCtx = makeOperationContext();
    const auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // Non-awaitable hello requests should return immediately.
    auto response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, {}, {});
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());

    // A request with a future TopologyVersion should error.
    const auto futureTopologyVersion = TopologyVersion(currentTopologyVersion.getProcessId(),
                                                       currentTopologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(futureTopologyVersion.getCounter(), currentTopologyVersion.getCounter());
    ASSERT_THROWS_CODE(
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, futureTopologyVersion, deadline),
        AssertionException,
        31382);

    // A request with a stale TopologyVersion should return immediately.
    const auto staleTopologyVersion = TopologyVersion(currentTopologyVersion.getProcessId(),
                                                      currentTopologyVersion.getCounter() - 1);
    ASSERT_LESS_THAN(staleTopologyVersion.getCounter(), currentTopologyVersion.getCounter());
    response = getReplCoord()->awaitHelloResponse(opCtx.get(), {}, staleTopologyVersion, deadline);
    auto responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());

    // A request with a different processId should return immediately with the server processId.
    const auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter());
    response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_FALSE(response->isSecondary());
    ASSERT_FALSE(response->isConfigSet());

    // A request with a future TopologyVersion but different processId should still return
    // immediately.
    topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter() + 1);
    ASSERT_GREATER_THAN(topologyVersionWithDifferentProcessId.getCounter(),
                        currentTopologyVersion.getCounter());
    response = getReplCoord()->awaitHelloResponse(
        opCtx.get(), {}, topologyVersionWithDifferentProcessId, deadline);
    responseTopologyVersion = response->getTopologyVersion();
    ASSERT_EQUALS(responseTopologyVersion->getProcessId(), currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
    ASSERT_FALSE(response->isWritablePrimary());

    AtomicWord<bool> helloReturned{false};
    // A request with an equal TopologyVersion should wait and timeout once the deadline is reached.
    const auto halfwayToDeadline = getNet()->now() + maxAwaitTime / 2;
    stdx::thread getHelloThread([&] {
        // Sending a hello request on a removed node should wait.
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);
        helloReturned.store(true);
        responseTopologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(responseTopologyVersion->getCounter(), currentTopologyVersion.getCounter());
        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_FALSE(response->isSecondary());
        ASSERT_FALSE(response->isConfigSet());
    });

    deadline = net->now() + maxAwaitTime;
    net->enterNetwork();
    // Set the network clock to a time before the deadline of the hello request. The request
    // should still be waiting.
    net->advanceTime(halfwayToDeadline);
    ASSERT_EQUALS(halfwayToDeadline, net->now());
    ASSERT_FALSE(helloReturned.load());

    // Set the network clock to the deadline.
    net->advanceTime(deadline);
    ASSERT_EQUALS(deadline, net->now());
    getHelloThread.join();
    ASSERT_TRUE(helloReturned.load());
    net->exitNetwork();
}

TEST_F(ReplCoordTest, AwaitHelloRespondsCorrectlyWhenNodeRemovedAndReadded) {
    init();
    const auto nodeOneHostName = "node1:12345";
    const auto nodeTwoHostName = "node2:12345";
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host" << nodeOneHostName << "_id" << 1)
                                          << BSON("host" << nodeTwoHostName << "_id" << 2))),
                       HostAndPort(nodeOneHostName));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    const auto maxAwaitTime = Milliseconds(5000);
    auto net = getNet();
    auto deadline = net->now() + maxAwaitTime;
    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    stdx::thread getHelloWaitingForRemovedNodeThread([&] {
        const auto topologyVersion = getTopoCoord().getTopologyVersion();
        // The hello response should indicate that the node does not have a valid replica set
        // config.
        const auto response = awaitHelloWithNewOpCtx(getReplCoord(), topologyVersion, {}, deadline);
        const auto responseTopologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(responseTopologyVersion->getProcessId(), topologyVersion.getProcessId());
        ASSERT_EQUALS(responseTopologyVersion->getCounter(), topologyVersion.getCounter() + 1);
        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_FALSE(response->isSecondary());
        ASSERT_FALSE(response->isConfigSet());
    });

    // Ensure that awaitHelloResponse() is called before triggering a reconfig.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort(nodeTwoHostName), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Receive a config that excludes node1 and with node2 having a configured horizon.
    ReplSetHeartbeatResponse hbResp;
    auto removedFromConfig =
        ReplSetConfig::parse(BSON("_id"
                                  << "mySet"
                                  << "protocolVersion" << 1 << "version" << 2 << "members"
                                  << BSON_ARRAY(BSON("host" << nodeTwoHostName << "_id" << 2
                                                            << "horizons"
                                                            << BSON("horizon1"
                                                                    << "testhorizon.com:100")))));
    hbResp.setConfig(removedFromConfig);
    hbResp.setConfigVersion(2);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
    net->runReadyNetworkOperations();
    exitNetwork();

    // node1 no longer exists in the replica set config.
    ASSERT_OK(getReplCoord()->waitForMemberState(opCtx.get(), MemberState::RS_REMOVED, Seconds(1)));
    ASSERT_EQUALS(removedFromConfig.getConfigVersion(), getReplCoord()->getConfigVersion());
    getHelloWaitingForRemovedNodeThread.join();
    const std::string newHorizonSniName = "newhorizon.com";
    auto newHorizon = SplitHorizon::Parameters(newHorizonSniName);

    stdx::thread getHelloThread([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        // Wait for the node to be readded to the set. This should return an error.
        ASSERT_THROWS_CODE(
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, {}, deadline),
            AssertionException,
            ErrorCodes::SplitHorizonChange);
    });
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 2);

    const auto newHorizonNodeOne = "newhorizon.com:100";
    const auto newHorizonNodeTwo = "newhorizon.com:200";

    // Add node1 back into the replica set and configure a new horizon.
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = true;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "protocolVersion" << 1 << "version" << 3 << "members"
                             << BSON_ARRAY(
                                    BSON("host" << nodeOneHostName << "_id" << 1 << "horizons"
                                                << BSON("newhorizon"
                                                        << "newhorizon.com:100"))
                                    << BSON("host" << nodeTwoHostName << "_id" << 2 << "horizons"
                                                   << BSON("newhorizon"
                                                           << "newhorizon.com:200"))));
    stdx::thread reconfigThread([&] {
        Status status(ErrorCodes::InternalError, "Not Set");
        status = getReplCoord()->processReplSetReconfig(opCtx.get(), args, &garbage);
        ASSERT_OK(status);
    });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_OK(
        getReplCoord()->waitForMemberState(opCtx.get(), MemberState::RS_SECONDARY, Seconds(1)));
    getHelloThread.join();

    stdx::thread getHelloThreadNewHorizon([&] {
        const auto expectedTopologyVersion = getTopoCoord().getTopologyVersion();
        // Sending a hello on the rejoined node should return the appropriate horizon view.
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), expectedTopologyVersion, newHorizon, deadline);
        HostAndPort expectedNodeOneHorizonView(newHorizonNodeOne);
        HostAndPort expectedNodeTwoHorizonView(newHorizonNodeTwo);
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(hosts[0], expectedNodeOneHorizonView);
        ASSERT_EQUALS(hosts[1], expectedNodeTwoHorizonView);
        ASSERT_EQUALS(response->getMe(), expectedNodeOneHorizonView);
    });

    deadline = getNet()->now() + maxAwaitTime;
    // Set the network clock to the timeout deadline of awaitHelloResponse.
    getNet()->enterNetwork();
    getNet()->advanceTime(deadline);
    ASSERT_EQUALS(deadline, getNet()->now());
    getHelloThreadNewHorizon.join();
    getNet()->exitNetwork();
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnElectionTimeout) {
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
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    // Wait for a hello with deadline past the election timeout.
    auto electionTimeout = getReplCoord()->getConfigElectionTimeoutPeriod();
    auto maxAwaitTime = electionTimeout + Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;
    auto electionTimeoutDate = getNet()->now() + electionTimeout;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A topology change should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;
    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);
        auto topologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedProcessId);

        ASSERT_FALSE(response->isWritablePrimary());
        ASSERT_TRUE(response->isSecondary());
        ASSERT_FALSE(response->hasPrimary());
    });

    // Ensure that awaitHelloResponse() is called before triggering an election timeout.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    getNet()->enterNetwork();
    // Primary steps down after not receiving a response within the election timeout.
    getNet()->advanceTime(electionTimeoutDate);
    getHelloThread.join();
    exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnElectionWin) {
    // The config does not have a "term" field, so step-up will not increment the config term
    // via reconfig. As a result, step-up only triggers two topology changes.
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

    // Become secondary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    ASSERT(getReplCoord()->getMemberState().secondary());

    auto maxAwaitTime = Milliseconds(50000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A topology change should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;

    auto opCtx = makeOperationContext();
    // Calling hello without a TopologyVersion field should return immediately.
    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_TRUE(response->isSecondary());
    ASSERT_FALSE(response->hasPrimary());

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto responseAfterElection =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);

        const auto topologyVersionAfterElection = responseAfterElection->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterElection->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterElection->getProcessId(), expectedProcessId);

        // We expect the server to increment the TopologyVersion and respond to waiting hellos
        // once an election is won even if we have yet to signal drain completion.
        ASSERT_FALSE(responseAfterElection->isWritablePrimary());
        ASSERT_TRUE(responseAfterElection->isSecondary());
        ASSERT_TRUE(responseAfterElection->hasPrimary());
        ASSERT_EQUALS(responseAfterElection->getPrimary().host(), "node1");
        ASSERT(getReplCoord()->getMemberState().primary());

        // The server TopologyVersion will increment again once we exit drain mode.
        expectedCounter = topologyVersionAfterElection->getCounter() + 1;
        const auto responseAfterDrainComplete = awaitHelloWithNewOpCtx(
            getReplCoord(), topologyVersionAfterElection.get(), {}, deadline);
        const auto topologyVersionAfterDrainComplete =
            responseAfterDrainComplete->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterDrainComplete->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterDrainComplete->getProcessId(), expectedProcessId);

        ASSERT_TRUE(responseAfterDrainComplete->isWritablePrimary());
        ASSERT_FALSE(responseAfterDrainComplete->isSecondary());
        ASSERT_TRUE(responseAfterDrainComplete->hasPrimary());
        ASSERT_EQUALS(responseAfterDrainComplete->getPrimary().host(), "node1");
        ASSERT(getReplCoord()->getMemberState().primary());
    });

    // Ensure that awaitHelloResponse() is called before finishing the election.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21505,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTimeoutWhen, opCtx.get());

    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 2);
    signalDrainComplete(opCtx.get());
    ASSERT(getReplCoord()->getApplierState() == ReplicationCoordinator::ApplierState::Stopped);

    getHelloThread.join();
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnElectionWinWithReconfig) {
    // The config has a "term" field, so step-up will increment the config term
    // via reconfig. As a result, step-up triggers three topology changes.
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "term" << 0 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    // Become secondary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    ASSERT(getReplCoord()->getMemberState().secondary());

    auto maxAwaitTime = Milliseconds(50000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A topology change should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;

    auto opCtx = makeOperationContext();
    // Calling hello without a TopologyVersion field should return immediately.
    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_TRUE(response->isSecondary());
    ASSERT_FALSE(response->hasPrimary());

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto hangAfterReconfigFailPoint =
        globalFailPointRegistry().find("hangAfterReconfigOnDrainComplete");

    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto responseAfterElection =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);

        const auto topologyVersionAfterElection = responseAfterElection->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterElection->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterElection->getProcessId(), expectedProcessId);

        // We expect the server to increment the TopologyVersion and respond to waiting hellos
        // once an election is won even if we have yet to signal drain completion.
        ASSERT_FALSE(responseAfterElection->isWritablePrimary());
        ASSERT_TRUE(responseAfterElection->isSecondary());
        ASSERT_TRUE(responseAfterElection->hasPrimary());
        ASSERT_EQUALS(responseAfterElection->getPrimary().host(), "node1");
        ASSERT(getReplCoord()->getMemberState().primary());

        // The server TopologyVersion will increment once we finish reconfig.
        expectedCounter = topologyVersionAfterElection->getCounter() + 1;
        const auto responseAfterReconfig = awaitHelloWithNewOpCtx(
            getReplCoord(), topologyVersionAfterElection.get(), {}, deadline);
        const auto topologyVersionAfterReconfig = responseAfterReconfig->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterReconfig->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterReconfig->getProcessId(), expectedProcessId);

        ASSERT_FALSE(responseAfterReconfig->isWritablePrimary());
        ASSERT_TRUE(responseAfterReconfig->isSecondary());
        ASSERT_TRUE(responseAfterReconfig->hasPrimary());
        ASSERT_EQUALS(responseAfterReconfig->getPrimary().host(), "node1");
        ASSERT(getReplCoord()->getMemberState().primary());

        hangAfterReconfigFailPoint->setMode(FailPoint::off);
        // The server TopologyVersion will increment again once we exit drain mode.
        expectedCounter = topologyVersionAfterReconfig->getCounter() + 1;
        const auto responseAfterDrainComplete = awaitHelloWithNewOpCtx(
            getReplCoord(), topologyVersionAfterReconfig.get(), {}, deadline);
        const auto topologyVersionAfterDrainComplete =
            responseAfterDrainComplete->getTopologyVersion();
        ASSERT_EQUALS(topologyVersionAfterDrainComplete->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersionAfterDrainComplete->getProcessId(), expectedProcessId);

        ASSERT_TRUE(responseAfterDrainComplete->isWritablePrimary());
        ASSERT_FALSE(responseAfterDrainComplete->isSecondary());
        ASSERT_TRUE(responseAfterDrainComplete->hasPrimary());
        ASSERT_EQUALS(responseAfterDrainComplete->getPrimary().host(), "node1");
        ASSERT(getReplCoord()->getMemberState().primary());
    });

    // Ensure that awaitHelloResponse() is called before finishing the election.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(4508104,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTimeoutWhen, opCtx.get());

    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 2);
    signalDrainComplete(opCtx.get());
    ASSERT(getReplCoord()->getApplierState() == ReplicationCoordinator::ApplierState::Stopped);

    getHelloThread.join();
}

TEST_F(ReplCoordTest, HelloResponseMentionsLackOfReplicaSetConfig) {
    start();

    auto opCtx = makeOperationContext();
    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(response->isConfigSet());
    BSONObj responseObj = response->toBSON();
    ASSERT_FALSE(responseObj["ismaster"].Bool());
    ASSERT_FALSE(responseObj["secondary"].Bool());
    ASSERT_TRUE(responseObj["isreplicaset"].Bool());
    ASSERT_EQUALS("Does not have a valid replica set config", responseObj["info"].String());

    HelloResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response->toBSON()));
}

TEST_F(ReplCoordTest, Hello) {
    HostAndPort h1("h1");
    HostAndPort h2("h2");
    HostAndPort h3("h3");
    HostAndPort h4("h4");
    assertStartSuccess(
        BSON(
            "_id"
            << "mySet"
            << "version" << 2 << "members"
            << BSON_ARRAY(BSON("_id" << 0 << "host" << h1.toString())
                          << BSON("_id" << 1 << "host" << h2.toString())
                          << BSON("_id" << 2 << "host" << h3.toString() << "arbiterOnly" << true)
                          << BSON("_id" << 3 << "host" << h4.toString() << "priority" << 0 << "tags"
                                        << BSON("key1"
                                                << "value1"
                                                << "key2"
                                                << "value2")))),
        h4);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    time_t lastWriteDate = 100;
    OpTime opTime = OpTime(Timestamp(lastWriteDate, 2), 1);
    replCoordSetMyLastAppliedOpTime(opTime, Date_t() + Seconds(100));

    auto opCtx = makeOperationContext();
    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);

    ASSERT_EQUALS("mySet", response->getReplSetName());
    ASSERT_EQUALS(2, response->getReplSetVersion());
    ASSERT_FALSE(response->isWritablePrimary());
    ASSERT_TRUE(response->isSecondary());
    // TODO(spencer): test that response includes current primary when there is one.
    ASSERT_FALSE(response->isArbiterOnly());
    ASSERT_TRUE(response->isPassive());
    ASSERT_FALSE(response->isHidden());
    ASSERT_TRUE(response->shouldBuildIndexes());
    ASSERT_EQUALS(Seconds(0), response->getSecondaryDelaySecs());
    ASSERT_EQUALS(h4, response->getMe());

    std::vector<HostAndPort> hosts = response->getHosts();
    ASSERT_EQUALS(2U, hosts.size());
    if (hosts[0] == h1) {
        ASSERT_EQUALS(h2, hosts[1]);
    } else {
        ASSERT_EQUALS(h2, hosts[0]);
        ASSERT_EQUALS(h1, hosts[1]);
    }
    std::vector<HostAndPort> passives = response->getPassives();
    ASSERT_EQUALS(1U, passives.size());
    ASSERT_EQUALS(h4, passives[0]);
    std::vector<HostAndPort> arbiters = response->getArbiters();
    ASSERT_EQUALS(1U, arbiters.size());
    ASSERT_EQUALS(h3, arbiters[0]);

    stdx::unordered_map<std::string, std::string> tags = response->getTags();
    ASSERT_EQUALS(2U, tags.size());
    ASSERT_EQUALS("value1", tags["key1"]);
    ASSERT_EQUALS("value2", tags["key2"]);
    ASSERT_EQUALS(opTime, response->getLastWriteOpTime());
    ASSERT_EQUALS(lastWriteDate, response->getLastWriteDate());

    HelloResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response->toBSON()));
}

TEST_F(ReplCoordTest, HelloWithCommittedSnapshot) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    time_t lastWriteDate = 101;
    OpTime opTime = OpTime(Timestamp(lastWriteDate, 2), 1);
    time_t majorityWriteDate = lastWriteDate;
    OpTime majorityOpTime = opTime;

    getStorageInterface()->allDurableTimestamp = opTime.getTimestamp();
    replCoordSetMyLastAppliedOpTime(opTime, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(opTime, Date_t() + Seconds(100));
    ASSERT_EQUALS(majorityOpTime, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    const auto response =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);

    ASSERT_EQUALS(opTime, response->getLastWriteOpTime());
    ASSERT_EQUALS(lastWriteDate, response->getLastWriteDate());
    ASSERT_EQUALS(majorityOpTime, response->getLastMajorityWriteOpTime());
    ASSERT_EQUALS(majorityWriteDate, response->getLastMajorityWriteDate());
}

TEST_F(ReplCoordTest, HelloInShutdown) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    const auto responseBeforeShutdown =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_TRUE(responseBeforeShutdown->isWritablePrimary());
    ASSERT_FALSE(responseBeforeShutdown->isSecondary());

    shutdown(opCtx.get());

    // Must not report ourselves as a writable primary while we're in shutdown.
    const auto responseAfterShutdown =
        getReplCoord()->awaitHelloResponse(opCtx.get(), {}, boost::none, boost::none);
    ASSERT_FALSE(responseAfterShutdown->isWritablePrimary());
    ASSERT_FALSE(responseBeforeShutdown->isSecondary());
}


TEST_F(ReplCoordTest, LogAMessageWhenShutDownBeforeReplicationStartUpFinished) {
    init();
    startCapturingLogMessages();
    {
        auto opCtx = makeOperationContext();
        getReplCoord()->shutdown(opCtx.get());
    }
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining("shutdown() called before startup() finished"));
}

TEST_F(ReplCoordTest, DoNotProcessSelfWhenUpdatePositionContainsInfoAboutSelf) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;

    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);

    // receive updatePosition containing ourself, should not process the update for self
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(
        BSON(UpdatePositionArgs::kCommandFieldName
             << 1 << UpdatePositionArgs::kUpdateArrayFieldName
             << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                << 2 << UpdatePositionArgs::kMemberIdFieldName << 0
                                << UpdatePositionArgs::kDurableOpTimeFieldName << time2.toBSON()
                                << UpdatePositionArgs::kDurableWallTimeFieldName
                                << Date_t() + Seconds(time2.getSecs())
                                << UpdatePositionArgs::kAppliedOpTimeFieldName << time2.toBSON()
                                << UpdatePositionArgs::kAppliedWallTimeFieldName
                                << Date_t() + Seconds(time2.getSecs()))))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest, ProcessUpdatePositionWhenItsConfigVersionIsDifferent) {
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
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;

    // receive updatePosition with a different config version, 3
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    auto updatePositionConfigVersion = 3;
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                           << updatePositionConfigVersion << UpdatePositionArgs::kMemberIdFieldName
                           << 1 << UpdatePositionArgs::kDurableOpTimeFieldName << time2.toBSON()
                           << UpdatePositionArgs::kDurableWallTimeFieldName
                           << Date_t() + Seconds(time2.getSecs())
                           << UpdatePositionArgs::kAppliedOpTimeFieldName << time2.toBSON()
                           << UpdatePositionArgs::kAppliedWallTimeFieldName
                           << Date_t() + Seconds(time2.getSecs()))))));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args));
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest, DoNotProcessUpdatePositionOfMembersWhoseIdsAreNotInTheConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;

    // receive updatePosition with nonexistent member id
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(
        BSON(UpdatePositionArgs::kCommandFieldName
             << 1 << UpdatePositionArgs::kUpdateArrayFieldName
             << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                << 2 << UpdatePositionArgs::kMemberIdFieldName << 9
                                << UpdatePositionArgs::kDurableOpTimeFieldName << time2.toBSON()
                                << UpdatePositionArgs::kDurableWallTimeFieldName
                                << Date_t() + Seconds(time2.getSecs())
                                << UpdatePositionArgs::kAppliedOpTimeFieldName << time2.toBSON()
                                << UpdatePositionArgs::kAppliedWallTimeFieldName
                                << Date_t() + Seconds(time2.getSecs()))))));

    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(ErrorCodes::NodeNotFound, getReplCoord()->processReplSetUpdatePosition(args));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest,
       ProcessUpdateWhenUpdatePositionContainsOnlyConfigVersionAndMemberIdsWithoutRIDs) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);
    OpTimeWithTermOne staleTime(10, 0);
    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time1, Date_t() + Seconds(100));

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.w = 1;

    // receive a good update position
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << 2 << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << time2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(time2.asOpTime().getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << time2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(time2.asOpTime().getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << 2 << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << time2.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(time2.asOpTime().getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << time2.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(time2.asOpTime().getSecs()))))));

    auto opCtx = makeOperationContext();


    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args));
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);

    writeConcern.w = 3;
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord, Status* status, bool force = false) {
    auto client = getGlobalServiceContext()->makeClient("rsr");
    auto opCtx = client->makeOperationContext();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = force;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 3)
                                           << BSON("_id" << 1 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(opCtx.get(), args, &garbage);
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnReplSetReconfig) {
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
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT(getReplCoord()->getMemberState().primary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A topology change should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;
    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);
        auto topologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedProcessId);

        // Ensure the HelloResponse contains the newly added node.
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(3, hosts.size());
        ASSERT_EQUALS("node3", hosts[2].host());
    });

    // Ensure that awaitHelloResponse() is called before triggering a reconfig.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Do a reconfig to add a third node to the replica set. A reconfig should cause the server to
    // respond to the waiting HelloResponse.
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(
        [&] { doReplSetReconfig(getReplCoord(), &status, true /* force */); });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_OK(status);
    getHelloThread.join();
}

TEST_F(ReplCoordTest, AwaitReplicationShouldResolveAsNormalDuringAReconfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));

    // Turn off readconcern majority support, and snapshots.
    disableReadConcernMajoritySupport();
    disableSnapshots();

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 2), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 2), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 3;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    ReplicationAwaiter awaiterJournaled(getReplCoord(), getServiceContext());
    writeConcern.w = WriteConcernOptions::kMajority;
    awaiterJournaled.setOpTime(time);
    awaiterJournaled.setWriteConcern(writeConcern);
    awaiterJournaled.start();

    // reconfig
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status, true); });

    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_OK(status);

    // satisfy write concern
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 0, time));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 1, time));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 2, time));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 0, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(3, 0, time));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 1, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(3, 1, time));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(3, 2, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(3, 2, time));
    ReplicationCoordinator::StatusAndDuration statusAndDurJournaled = awaiterJournaled.getResult();
    ASSERT_OK(statusAndDurJournaled.status);
    awaiterJournaled.reset();
}

void doReplSetReconfigToFewer(ReplicationCoordinatorImpl* replCoord, Status* status, bool force) {
    auto client = getGlobalServiceContext()->makeClient("rsr");
    auto opCtx = client->makeOperationContext();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = force;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "protocolVersion" << 1 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(opCtx.get(), args, &garbage);
}

TEST_F(ReplCoordTest, AwaitHelloResponseReturnsOnReplSetReconfigOnSecondary) {
    init();
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));

    // Become secondary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    ASSERT(getReplCoord()->getMemberState().secondary());

    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = getNet()->now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto expectedProcessId = currentTopologyVersion.getProcessId();
    // A topology change should increment the TopologyVersion counter.
    auto expectedCounter = currentTopologyVersion.getCounter() + 1;
    auto opCtx = makeOperationContext();

    auto waitForHelloFailPoint = globalFailPointRegistry().find("waitForHelloResponse");
    auto timesEnteredFailPoint = waitForHelloFailPoint->setMode(FailPoint::alwaysOn, 0);
    ON_BLOCK_EXIT([&] { waitForHelloFailPoint->setMode(FailPoint::off, 0); });

    // awaitHelloResponse blocks and waits on a future when the request TopologyVersion equals
    // the current TopologyVersion of the server.
    stdx::thread getHelloThread([&] {
        const auto response =
            awaitHelloWithNewOpCtx(getReplCoord(), currentTopologyVersion, {}, deadline);
        auto topologyVersion = response->getTopologyVersion();
        ASSERT_EQUALS(topologyVersion->getCounter(), expectedCounter);
        ASSERT_EQUALS(topologyVersion->getProcessId(), expectedProcessId);

        // Ensure the HelloResponse no longer contains the removed node.
        const auto hosts = response->getHosts();
        ASSERT_EQUALS(2, hosts.size());
        ASSERT_EQUALS("node1", hosts[0].host());
        ASSERT_EQUALS("node3", hosts[1].host());
    });

    // Ensure that awaitHelloResponse() is called before triggering a reconfig.
    waitForHelloFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Do a reconfig to remove a node from the replica set. A reconfig should cause the server to
    // respond to the waiting hello request.
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(
        [&] { doReplSetReconfigToFewer(getReplCoord(), &status, true /* force */); });
    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_OK(status);
    getHelloThread.join();
}

TEST_F(
    ReplCoordTest,
    NodeReturnsUnsatisfiableWriteConcernWhenReconfiggingToAClusterThatCannotSatisfyTheWriteConcern) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 2), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 2), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = 3;

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    // reconfig to fewer nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(
        [&] { doReplSetReconfigToFewer(getReplCoord(), &status, true /* force */); });

    replyToReceivedHeartbeatV1();

    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and an error should be returned
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::UnsatisfiableWriteConcern, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsOKFromAwaitReplicationWhenReconfiggingToASetWhereMajorityIsSmallerAndSatisfied) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4))),
                       HostAndPort("node1", 12345));

    // Turn off readconcern majority support, and snapshots.
    disableReadConcernMajoritySupport();
    disableSnapshots();

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    OpTime time(Timestamp(100, 2), 1);
    auto opCtx = makeOperationContext();

    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time));


    // majority nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.w = WriteConcernOptions::kMajority;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;


    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    // demonstrate that majority cannot currently be satisfied
    WriteConcernOptions writeConcern2;
    writeConcern2.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern2.w = WriteConcernOptions::kMajority;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern2).status);

    // reconfig to three nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(
        [&] { doReplSetReconfig(getReplCoord(), &status, true /* force */); });

    replyToReceivedHeartbeatV1();
    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and be satisfied
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsFromMajorityWriteConcernOnlyOnceTheWriteAppearsInACommittedSnapShot) {
    // Test that we can satisfy majority write concern can only be
    // satisfied by voting data-bearing members.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0 << "priority" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime time(Timestamp(100, 1), 1);
    getStorageInterface()->allDurableTimestamp = time.getTimestamp();
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.w = WriteConcernOptions::kMajority;
    majorityWriteConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;

    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 1, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time, majorityWriteConcern).status);

    // this member does not vote and as a result should not count towards write concern
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 3, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time));
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time, majorityWriteConcern).status);
}

TEST_F(ReplCoordTest,
       UpdateLastCommittedOpTimeWhenAndOnlyWhenAMajorityOfVotingNodesHaveReceivedTheOp) {
    // Test that the commit level advances properly.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0 << "priority" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime zero(Timestamp(0, 0), 0);
    OpTime time(Timestamp(100, 1), 1);
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 1, time));
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 3, time));
    ASSERT_EQUALS(zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time));
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());


    // Set a new, later OpTime.
    OpTime newTime(Timestamp(100, 1), 1);
    replCoordSetMyLastAppliedOpTime(newTime, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(newTime, Date_t() + Seconds(100));
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, newTime));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 3, newTime));
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, newTime));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, newTime));
    // Reached majority of voting nodes with newTime.
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, newTime));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 1, newTime));
    ASSERT_EQUALS(newTime, getReplCoord()->getLastCommittedOpTime());
}

/**
 * Tests to ensure that ReplicationCoordinator correctly calculates and updates the stable
 * optime.
 */
class StableOpTimeTest : public ReplCoordTest {};

TEST_F(StableOpTimeTest, SetMyLastAppliedSetsStableOpTimeForStorage) {

    /**
     * Test that 'setMyLastAppliedOpTime' sets the stable timestamp properly for the storage engine.
     */
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));

    Timestamp stableTimestamp;

    ASSERT_EQUALS(Timestamp::min(), getStorageInterface()->getStableTimestamp());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    getStorageInterface()->allDurableTimestamp = Timestamp(1, 1);
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(1, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(1, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    // Advance the commit point so it's higher than all the others.
    replCoordAdvanceCommitPoint(OpTimeWithTermOne(10, 1), Date_t() + Seconds(100), false);
    ASSERT_EQUALS(Timestamp(1, 1), getStorageInterface()->getStableTimestamp());

    // Check that the stable timestamp is not updated if the all durable timestamp is behind.
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(1, 2), Date_t() + Seconds(100));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(1, 1), getStorageInterface()->getStableTimestamp());

    getStorageInterface()->allDurableTimestamp = Timestamp(3, 1);

    // Check that the stable timestamp is updated for the storage engine when we set the applied
    // optime.
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(2, 1), Date_t() + Seconds(100));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    // Check that timestamp cleanup occurs.
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(2, 2), Date_t() + Seconds(100));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 2), stableTimestamp);
}

TEST_F(StableOpTimeTest, AdvanceCommitPointSetsStableOpTimeForStorage) {

    /**
     * Test that 'advanceCommitPoint' sets the stable optime for the storage engine.
     */

    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(1, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(1, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    Timestamp stableTimestamp;
    long long term = 1;

    getStorageInterface()->allDurableTimestamp = Timestamp(2, 1);

    // Add three stable optime candidates.
    replCoordSetMyLastAppliedOpTime(OpTime({2, 1}, term), Date_t() + Seconds(1));
    replCoordSetMyLastAppliedOpTime(OpTime({2, 2}, term), Date_t() + Seconds(2));
    replCoordSetMyLastAppliedOpTime(OpTime({3, 2}, term), Date_t() + Seconds(3));

    // Set a commit point and check the stable optime.
    replCoordAdvanceCommitPoint(OpTime({2, 1}, term), Date_t() + Seconds(1), false);
    ASSERT_EQUALS(getReplCoord()->getLastCommittedOpTimeAndWallTime().wallTime,
                  Date_t() + Seconds(1));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    // Check that the stable timestamp is not updated if the all durable timestamp is behind.
    replCoordAdvanceCommitPoint(OpTime({2, 2}, term), Date_t() + Seconds(2), false);
    ASSERT_EQUALS(getReplCoord()->getLastCommittedOpTimeAndWallTime().wallTime,
                  Date_t() + Seconds(2));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    getStorageInterface()->allDurableTimestamp = Timestamp(4, 4);

    // Check that the stable timestamp is updated when we advance the commit point.
    replCoordAdvanceCommitPoint(OpTime({3, 2}, term), Date_t() + Seconds(3), false);
    ASSERT_EQUALS(getReplCoord()->getLastCommittedOpTimeAndWallTime().wallTime,
                  Date_t() + Seconds(3));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(3, 2), stableTimestamp);
}

TEST_F(ReplCoordTest, NodeReturnsShutdownInProgressWhenWaitingUntilAnOpTimeDuringShutdown) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(10, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(10, 1), Date_t() + Seconds(100));

    auto opCtx = makeOperationContext();

    shutdown(opCtx.get());

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTimeWithTermOne(50, 0), ReadConcernLevel::kLocalReadConcern));
    ASSERT_EQ(status, ErrorCodes::ShutdownInProgress);
}

TEST_F(ReplCoordTest, NodeReturnsInterruptedWhenWaitingUntilAnOpTimeIsInterrupted) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(10, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(10, 1), Date_t() + Seconds(100));

    const auto opCtx = makeOperationContext();
    killOperation(opCtx.get());

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTimeWithTermOne(50, 0), ReadConcernLevel::kLocalReadConcern));
    ASSERT_EQ(status, ErrorCodes::Interrupted);
}

TEST_F(ReplCoordTest, NodeReturnsOkImmediatelyWhenWaitingUntilOpTimePassesNoOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(opCtx.get(), ReadConcernArgs()));
}

TEST_F(ReplCoordTest, NodeReturnsOkImmediatelyWhenWaitingUntilOpTimePassesAnOpTimePriorToOurLast) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTimeWithTermOne(50, 0), ReadConcernLevel::kLocalReadConcern)));
}

TEST_F(ReplCoordTest, NodeReturnsOkImmediatelyWhenWaitingUntilOpTimePassesAnOpTimeEqualToOurLast) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTimeWithTermOne time(100, 1);
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(), ReadConcernArgs(time, ReadConcernLevel::kLocalReadConcern)));
}

TEST_F(ReplCoordTest,
       NodeReturnsNotAReplicaSetWhenWaitUntilOpTimeIsRunWithoutMajorityReadConcernEnabled) {
    init(ReplSettings());

    auto opCtx = makeOperationContext();

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTimeWithTermOne(50, 0), ReadConcernLevel::kLocalReadConcern));
    ASSERT_EQ(status, ErrorCodes::NotAReplicaSet);
}

TEST_F(ReplCoordTest, NodeReturnsNotAReplicaSetWhenWaitUntilOpTimeIsRunAgainstAStandaloneNode) {
    init(ReplSettings());

    auto opCtx = makeOperationContext();

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));
    ASSERT_EQ(status, ErrorCodes::NotAReplicaSet);
}

// TODO(dannenberg): revisit these after talking with mathias (redundant with other set?)
TEST_F(ReplCoordTest, ReadAfterCommittedWhileShutdown) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(10, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(10, 1), 0), Date_t() + Seconds(100));

    shutdown(opCtx.get());

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));
    ASSERT_EQUALS(status, ErrorCodes::ShutdownInProgress);
}

TEST_F(ReplCoordTest, ReadAfterCommittedInterrupted) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    const auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(10, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(10, 1), 0), Date_t() + Seconds(100));
    killOperation(opCtx.get());
    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));
    ASSERT_EQUALS(status, ErrorCodes::Interrupted);
}

TEST_F(ReplCoordTest, ReadAfterCommittedGreaterOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    getStorageInterface()->allDurableTimestamp = Timestamp(100, 1);
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 1), ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedEqualOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    OpTime time(Timestamp(100, 1), 1);
    getStorageInterface()->allDurableTimestamp = time.getTimestamp();
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(), ReadConcernArgs(time, ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredGreaterOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());
    getStorageInterface()->allDurableTimestamp = Timestamp(100, 1);
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
    OpTime committedOpTime(Timestamp(200, 1), 1);
    auto pseudoLogOp = stdx::async(stdx::launch::async, [this, &committedOpTime]() {
        // Not guaranteed to be scheduled after waitUntil blocks...
        replCoordSetMyLastAppliedOpTime(committedOpTime, Date_t() + Seconds(100));
        replCoordSetMyLastDurableOpTime(committedOpTime, Date_t() + Seconds(100));
    });

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(100, 0), 1), ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredEqualOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(100, 1), 1), Date_t() + Seconds(100));

    OpTime opTimeToWait(Timestamp(100, 1), 1);

    auto pseudoLogOp = stdx::async(stdx::launch::async, [this, &opTimeToWait]() {
        // Not guaranteed to be scheduled after waitUntil blocks...
        getStorageInterface()->allDurableTimestamp = opTimeToWait.getTimestamp();
        replCoordSetMyLastAppliedOpTime(opTimeToWait, Date_t() + Seconds(100));
        replCoordSetMyLastDurableOpTime(opTimeToWait, Date_t() + Seconds(100));
    });

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(), ReadConcernArgs(opTimeToWait, ReadConcernLevel::kMajorityReadConcern)));
    pseudoLogOp.get();
}


TEST_F(ReplCoordTest, WaitUntilOpTimeforReadReturnsImmediatelyForMajorityReadConcern) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();

    // A valid majority read concern level should return immediately.
    auto rcArgs = ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern);
    auto status = getReplCoord()->waitUntilOpTimeForRead(opCtx.get(), rcArgs);
    ASSERT_OK(status);
}

TEST_F(ReplCoordTest, DoNotIgnoreTheContentsOfMetadataWhenItsConfigVersionDoesNotMatchOurs) {
    // Ensure that we do not process ReplSetMetadata when ConfigVersions do not match.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // lower configVersion
    auto lowerConfigVersion = 1;
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 2LL) << "lastCommittedWall"
                              << Date_t() + Seconds(100) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 2LL) << "configVersion"
                              << lowerConfigVersion << "configTerm" << 2 << "term" << 2
                              << "syncSourceIndex" << 1 << "isPrimary" << true)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    // term should advance
    ASSERT_EQUALS(2, getReplCoord()->getTerm());

    // higher configVersion
    auto higherConfigVersion = 100;
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 2LL) << "lastCommittedWall"
                              << Date_t() + Seconds(100) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 2LL) << "configVersion"
                              << higherConfigVersion << "configTerm" << 2 << "term" << 2
                              << "syncSourceIndex" << 1 << "isPrimary" << true)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    // term should advance
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
}

TEST_F(ReplCoordTest, UpdateLastCommittedOpTimeWhenTheLastCommittedOpTimeIsNewer) {
    // Ensure that LastCommittedOpTime updates when a newer OpTime comes in via ReplSetMetadata,
    // but not if the OpTime is older than the current LastCommittedOpTime.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    OpTime time(Timestamp(10, 1), 1);
    OpTime oldTime(Timestamp(9, 1), 1);
    Date_t wallTime = Date_t() + Seconds(10);
    replCoordSetMyLastAppliedOpTime(time, wallTime);

    // higher OpTime, should change
    getReplCoord()->advanceCommitPoint({time, wallTime}, false);
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(wallTime, getReplCoord()->getLastCommittedOpTimeAndWallTime().wallTime);
    ASSERT_EQUALS(time, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // lower OpTime, should not change
    getReplCoord()->advanceCommitPoint({oldTime, Date_t() + Seconds(5)}, false);
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(wallTime, getReplCoord()->getLastCommittedOpTimeAndWallTime().wallTime);
    ASSERT_EQUALS(time, getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest, UpdateTermWhenTheTermFromMetadataIsNewerButNeverUpdateCurrentPrimaryIndex) {
    // Ensure that the term is updated if and only if the term is greater than our current term.
    // Ensure that currentPrimaryIndex is never altered by ReplSetMetadata.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    // Higher term, should change.
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 3LL)
                                  << "lastCommittedWall" << Date_t() + Seconds(100)
                                  << "lastOpVisible" << BSON("ts" << Timestamp(10, 0) << "t" << 3LL)
                                  << "configVersion" << 2 << "configTerm" << 2 << "term" << 3
                                  << "syncSourceIndex" << 1 << "isPrimary" << true)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // Lower term, should not change.
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3LL)
                                  << "lastCommittedWall" << Date_t() + Seconds(100)
                                  << "lastOpVisible" << BSON("ts" << Timestamp(11, 0) << "t" << 3LL)
                                  << "configVersion" << 2 << "configTerm" << 2 << "term" << 2
                                  << "syncSourceIndex" << 1 << "isPrimary" << true)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // Same term, should not change.
    StatusWith<rpc::ReplSetMetadata> metadata3 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3LL)
                                  << "lastCommittedWall" << Date_t() + Seconds(100)
                                  << "lastOpVisible" << BSON("ts" << Timestamp(11, 0) << "t" << 3LL)
                                  << "configVersion" << 2 << "configTerm" << 2 << "term" << 3
                                  << "syncSourceIndex" << 1 << "isPrimary" << true)));
    getReplCoord()->processReplSetMetadata(metadata3.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());

    // Metadata docs include isPrimary: true, but we do NOT update currentPrimaryIndex.
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest,
       LastCommittedOpTimeNotUpdatedEvenWhenHeartbeatResponseWithMetadataHasFresherValues) {
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
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    auto replCoord = getReplCoord();
    auto config = replCoord->getConfig();

    // Higher term - should update term but not last committed optime.
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 3LL) << "lastCommittedWall"
                              << Date_t() + Seconds(100) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 0) << "t" << 3LL) << "configVersion"
                              << config.getConfigVersion() << "configTerm" << config.getConfigTerm()
                              << "term" << 3 << "syncSourceIndex" << 1 << "isPrimary" << true)));
    BSONObjBuilder responseBuilder;
    ASSERT_OK(metadata.getValue().writeToMetadata(&responseBuilder));

    auto net = getNet();
    net->enterNetwork();

    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    const auto& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(config.getConfigVersion());
    hbResp.setSetName(config.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    responseBuilder.appendElements(hbResp.toBSON());
    net->scheduleResponse(noi, net->now(), makeResponseStatus(responseBuilder.obj()));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
}

TEST_F(ReplCoordTest, AdvanceCommitPointFromSyncSourceCanSetCommitPointToLastAppliedIgnoringTerm) {
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

    OpTimeAndWallTime lastApplied = {OpTime({10, 1}, 1), Date_t() + Seconds(10)};
    OpTimeAndWallTime commitPoint = {OpTime({15, 1}, 2), Date_t() + Seconds(15)};
    replCoordSetMyLastAppliedOpTime(lastApplied.opTime, lastApplied.wallTime);

    const bool fromSyncSource = true;
    getReplCoord()->advanceCommitPoint(commitPoint, fromSyncSource);

    // The commit point can be set to lastApplied, even though lastApplied is in a lower term.
    ASSERT_EQUALS(lastApplied.opTime, getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest, PrepareOplogQueryMetadata) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "term" << 0 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    OpTime optime1{Timestamp(10, 0), 5};
    OpTime optime2{Timestamp(11, 2), 5};
    Date_t wallTime1 = Date_t() + Seconds(1);
    Date_t wallTime2 = Date_t() + Seconds(2);

    replCoordSetMyLastAppliedOpTime(optime2, wallTime2);
    // pass dummy Date_t to avoid advanceCommitPoint invariant
    getReplCoord()->advanceCommitPoint({optime1, wallTime1}, false);

    auto opCtx = makeOperationContext();

    BSONObjBuilder metadataBob;
    getReplCoord()->prepareReplMetadata(
        BSON(rpc::kOplogQueryMetadataFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
        OpTime(),
        &metadataBob);

    BSONObj metadata = metadataBob.done();
    LOGV2(21506, "{metadata}", "metadata"_attr = metadata);

    auto oqMetadata = rpc::OplogQueryMetadata::readFromMetadata(metadata);
    ASSERT_OK(oqMetadata.getStatus());
    ASSERT_EQ(oqMetadata.getValue().getLastOpCommitted().opTime, optime1);
    ASSERT_EQ(oqMetadata.getValue().getLastOpCommitted().wallTime, wallTime1);
    ASSERT_EQ(oqMetadata.getValue().getLastOpApplied(), optime2);
    ASSERT_EQ(oqMetadata.getValue().getRBID(), 100);
    ASSERT_EQ(oqMetadata.getValue().getSyncSourceIndex(), -1);
    ASSERT_EQ(oqMetadata.getValue().hasPrimaryIndex(), false);

    auto replMetadata = rpc::ReplSetMetadata::readFromMetadata(metadata);
    ASSERT_OK(replMetadata.getStatus());
    ASSERT_EQ(replMetadata.getValue().getLastOpCommitted().opTime, optime1);
    ASSERT_EQ(replMetadata.getValue().getLastOpCommitted().wallTime, wallTime1);
    ASSERT_EQ(replMetadata.getValue().getLastOpVisible(), optime1);
    ASSERT_EQ(replMetadata.getValue().getConfigVersion(), 2);
    ASSERT_EQ(replMetadata.getValue().getConfigTerm(), 0);
    ASSERT_EQ(replMetadata.getValue().getTerm(), 0);
    ASSERT_EQ(replMetadata.getValue().getSyncSourceIndex(), -1);
    ASSERT_EQ(replMetadata.getValue().getIsPrimary(), false);
}

TEST_F(ReplCoordTest, TermAndLastCommittedOpTimeUpdatedFromHeartbeatWhenArbiter) {
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0 << "arbiterOnly" << true)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))
                            << "protocolVersion" << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    auto replCoord = getReplCoord();
    auto config = replCoord->getConfig();

    // Higher term - should update term and lastCommittedOpTime since arbiters learn of the
    // commit point via heartbeats.
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName << BSON(
            "lastOpCommitted" << BSON("ts" << Timestamp(10, 1) << "t" << 3LL) << "lastCommittedWall"
                              << Date_t() + Seconds(100) << "lastOpVisible"
                              << BSON("ts" << Timestamp(10, 1) << "t" << 3LL) << "configVersion"
                              << config.getConfigVersion() << "configTerm" << config.getConfigTerm()
                              << "term" << 3 << "syncSourceIndex" << 1 << "isPrimary" << true)));
    BSONObjBuilder responseBuilder;
    ASSERT_OK(metadata.getValue().writeToMetadata(&responseBuilder));

    auto net = getNet();
    net->enterNetwork();

    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    const auto& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(config.getConfigVersion());
    hbResp.setSetName(config.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    responseBuilder.appendElements(hbResp.toBSON());
    net->scheduleResponse(noi, net->now(), makeResponseStatus(responseBuilder.obj()));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(OpTime(Timestamp(10, 1), 3), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
}

TEST_F(ReplCoordTest,
       ScheduleElectionToBeRunInElectionTimeoutFromNowWhenCancelAndRescheduleElectionTimeoutIsRun) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();


    auto net = getNet();
    net->enterNetwork();

    // Black hole heartbeat request scheduled after transitioning to SECONDARY.
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    const auto& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
    LOGV2(21507,
          "black holing {noi_getRequest_cmdObj}",
          "noi_getRequest_cmdObj"_attr = noi->getRequest().cmdObj);
    net->blackHole(noi);

    // Advance simulator clock to some time before the first scheduled election.
    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    LOGV2(21508,
          "Election initially scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);
    ASSERT_GREATER_THAN(electionTimeoutWhen, net->now());
    auto until = net->now() + (electionTimeoutWhen - net->now()) / 2;
    net->runUntil(until);
    ASSERT_EQUALS(until, net->now());
    net->exitNetwork();

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_LESS_THAN_OR_EQUALS(until + replCoord->getConfigElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, DoNotScheduleElectionWhenCancelAndRescheduleElectionTimeoutIsRunInRollback) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();

    // We must take the RSTL in mode X before transitioning to RS_ROLLBACK.
    const auto opCtx = makeOperationContext();
    ReplicationStateTransitionLockGuard transitionGuard(opCtx.get(), MODE_X);
    ASSERT_OK(replCoord->setFollowerModeRollback(opCtx.get()));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest,
       DoNotScheduleElectionWhenCancelAndRescheduleElectionTimeoutIsRunWhileUnelectable) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0 << "priority" << 0 << "hidden" << true)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest,
       DoNotScheduleElectionWhenCancelAndRescheduleElectionTimeoutIsRunWhileRemoved) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    LOGV2(21509, "processing {request_cmdObj}", "request_cmdObj"_attr = request.cmdObj);
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command with a config that excludes node1.
    ReplSetHeartbeatResponse hbResp;
    auto config = ReplSetConfig::parse(BSON("_id"
                                            << "mySet"
                                            << "protocolVersion" << 1 << "version" << 3 << "members"
                                            << BSON_ARRAY(BSON("host"
                                                               << "node2:12345"
                                                               << "_id" << 1))));
    hbResp.setConfig(config);
    hbResp.setConfigVersion(3);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(getReplCoord()->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_REMOVED, Seconds(1)));
    ASSERT_EQUALS(config.getConfigVersion(), getReplCoord()->getConfigVersion());

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_EQUALS(Date_t(), replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       RescheduleElectionTimeoutWhenProcessingHeartbeatResponseFromPrimaryInSameTerm) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    LOGV2(21510, "processing {request_cmdObj}", "request_cmdObj"_attr = request.cmdObj);
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setTerm(replCoord->getTerm());
    hbResp.setAppliedOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setDurableOpTimeAndWallTime({OpTime(Timestamp(100, 1), 0), Date_t() + Seconds(100)});
    hbResp.setConfigVersion(1);

    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON()));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_LESS_THAN_OR_EQUALS(heartbeatWhen + replCoord->getConfigElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       DontRescheduleElectionTimeoutWhenProcessingHeartbeatResponseFromPrimaryInDiffertTerm) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));
    // Disable randomized election timeouts so that we know exactly when an election will be
    // scheduled.
    getExternalState()->setElectionTimeoutOffsetLimitFraction(0);
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    LOGV2(21511, "processing {request_cmdObj}", "request_cmdObj"_attr = request.cmdObj);
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setTerm(replCoord->getTerm() - 1);

    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON()));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_GREATER_THAN(heartbeatWhen + replCoord->getConfigElectionTimeoutPeriod(),
                        replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       CancelAndRescheduleElectionTimeoutWhenProcessingHeartbeatResponseWithoutState) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);

    auto net = getNet();
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    auto&& request = noi->getRequest();
    LOGV2(21512, "processing {request_cmdObj}", "request_cmdObj"_attr = request.cmdObj);
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON()));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    // Election timeout should remain unchanged.
    ASSERT_EQUALS(electionTimeoutWhen, replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, CancelAndRescheduleElectionTimeoutLogging) {
    // Log all the election messages.
    auto replElectionAllSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplicationElection, logv2::LogSeverity::Debug(5)};
    startCapturingLogMessages();
    // heartbeatTimeoutSecs is made large so we can advance the clock without worrying about
    // additional heartbeats.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "settings" << BSON("heartbeatTimeoutSecs" << 60000)
                            << "protocolVersion" << 1 << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1))),
                       HostAndPort("node1", 12345));

    // Setting mode to secondary should schedule the election timeout.
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Scheduled election timeout callback"));
    ASSERT_EQ(0, countTextFormatLogLinesContaining("Rescheduled election timeout callback"));
    ASSERT_EQ(0, countTextFormatLogLinesContaining("Canceling election timeout callback"));

    // Scheduling again should produce the "rescheduled", not the "scheduled", message .
    replCoord->cancelAndRescheduleElectionTimeout();
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Scheduled election timeout callback"));
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Rescheduled election timeout callback"));

    auto net = getNet();
    net->enterNetwork();

    // Black hole heartbeat request scheduled after transitioning to SECONDARY.
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    const auto& request = noi->getRequest();
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());
    LOGV2(21513,
          "black holing {noi_getRequest_cmdObj}",
          "noi_getRequest_cmdObj"_attr = noi->getRequest().cmdObj);
    net->blackHole(noi);

    // Advance simulator clock to some time after the first scheduled election.
    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    LOGV2(21514,
          "Election initially scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);
    ASSERT_GREATER_THAN(electionTimeoutWhen, net->now());
    auto until = electionTimeoutWhen + Milliseconds(1);
    net->runUntil(until);
    ASSERT_EQUALS(until, net->now());
    net->exitNetwork();

    // The election should have scheduled (not rescheduled) another timeout.
    ASSERT_EQ(2, countTextFormatLogLinesContaining("Scheduled election timeout callback"));
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Rescheduled election timeout callback"));

    auto replElectionReducedSeverityGuard = unittest::MinimumLoggedSeverityGuard{
        logv2::LogComponent::kReplicationElection, logv2::LogSeverity::Debug(4)};
    net->enterNetwork();
    until = electionTimeoutWhen + Milliseconds(500);
    net->runUntil(until);
    net->exitNetwork();
    replCoord->cancelAndRescheduleElectionTimeout();

    // We should not see this reschedule because it should be at log level 5.
    ASSERT_EQ(2, countTextFormatLogLinesContaining("Scheduled election timeout callback"));
    ASSERT_EQ(1, countTextFormatLogLinesContaining("Rescheduled election timeout callback"));

    net->enterNetwork();
    until = electionTimeoutWhen + Milliseconds(1001);
    net->runUntil(until);
    net->exitNetwork();
    replCoord->cancelAndRescheduleElectionTimeout();

    stopCapturingLogMessages();
    // We should see this reschedule at level 4 because it has been over 1 sec since we logged
    // at level 4.
    ASSERT_EQ(2, countTextFormatLogLinesContaining("Scheduled election timeout callback"));
    ASSERT_EQ(2, countTextFormatLogLinesContaining("Rescheduled election timeout callback"));
}

TEST_F(ReplCoordTest, ZeroCommittedSnapshotAfterClearingCommittedSnapshot) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);
    OpTime time3(Timestamp(100, 3), 1);
    OpTime time4(Timestamp(100, 4), 1);
    OpTime time5(Timestamp(100, 5), 1);
    OpTime time6(Timestamp(100, 6), 1);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    replCoordSetMyLastAppliedOpTime(time5, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time5, Date_t() + Seconds(100));

    getReplCoord()->clearCommittedSnapshot();
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest, DoNotAdvanceCommittedSnapshotWhenAppliedOpTimeChanges) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
    replCoordSetMyLastAppliedOpTime(time2, Date_t() + Seconds(100));
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getStorageInterface()->allDurableTimestamp = time2.getTimestamp();
    replCoordSetMyLastDurableOpTime(time2, Date_t() + Seconds(100));
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest,
       NodeChangesMyLastOpTimeWhenAndOnlyWhensetMyLastDurableOpTimeReceivesANewerOpTime4DurableSE) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    auto term = getTopoCoord().getTerm();
    OpTime time1(Timestamp(100, 1), term);
    OpTime time2(Timestamp(100, 2), term);
    OpTime time3(Timestamp(100, 3), term);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    replCoordSetMyLastAppliedOpTimeForward(time3, Date_t() + Seconds(100));
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastAppliedOpTime());
    replCoordSetMyLastAppliedOpTimeForward(time2, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTimeForward(time2, Date_t() + Seconds(100));
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastAppliedOpTime());
}

DEATH_TEST_F(ReplCoordTest,
             SetMyLastOpTimeToTimestampLesserThanCurrentLastOpTimeTimestampButWithHigherTerm,
             "opTime.getTimestamp() > myLastAppliedOpTime.getTimestamp()") {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(99, 1), 2);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    // Since in pv1, oplog entries are ordered by non-decreasing
    // term and strictly increasing timestamp, it leads to invariant failure.
    replCoordSetMyLastAppliedOpTimeForward(time2, Date_t() + Seconds(100));
}

DEATH_TEST_F(ReplCoordTest,
             SetMyLastOpTimeToTimestampEqualToCurrentLastOpTimeTimestampButWithHigherTerm,
             "opTime.getTimestamp() > myLastAppliedOpTime.getTimestamp()") {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 1), 2);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    // Since in pv1, oplog entries are ordered by non-decreasing
    // term and strictly increasing timestamp, it leads to invariant failure.
    replCoordSetMyLastAppliedOpTimeForward(time2, Date_t() + Seconds(100));
}

DEATH_TEST_F(ReplCoordTest,
             SetMyLastOpTimeToTimestampGreaterThanCurrentLastOpTimeTimestampButWithLesserTerm,
             "opTime.getTimestamp() < myLastAppliedOpTime.getTimestamp()") {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 0);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    // Since in pv1, oplog entries are ordered by non-decreasing
    // term and strictly increasing timestamp, it leads to invariant failure.
    replCoordSetMyLastAppliedOpTimeForward(time2, Date_t() + Seconds(100));
}

DEATH_TEST_F(ReplCoordTest,
             SetMyLastOpTimeToTimestampEqualToCurrentLastOpTimeTimestampButWithLesserTerm,
             "opTime.getTimestamp() < myLastAppliedOpTime.getTimestamp()") {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 1), 0);

    replCoordSetMyLastAppliedOpTime(time1, Date_t() + Seconds(100));
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    // Since in pv1, oplog entries are ordered by non-decreasing
    // term and strictly increasing timestamp, it leads to invariant failure.
    replCoordSetMyLastAppliedOpTimeForward(time2, Date_t() + Seconds(100));
}

TEST_F(ReplCoordTest, OnlyForwardSyncProgressForOtherNodesWhenTheNodesAreBelievedToBeUp) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234")
                           << BSON("_id" << 2 << "host"
                                         << "test3:1234"))
             << "protocolVersion" << 1 << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("test1", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    OpTime optime(Timestamp(100, 2), 0);
    replCoordSetMyLastAppliedOpTime(optime, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime, Date_t() + Seconds(100));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(1, 1, optime));

    // Check that we have two entries in our UpdatePosition (us and node 1).
    BSONObj cmd = unittest::assertGet(getReplCoord()->prepareReplSetUpdatePositionCommand());
    std::set<long long> memberIds;
    BSONForEach(entryElement, cmd[UpdatePositionArgs::kUpdateArrayFieldName].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry[UpdatePositionArgs::kMemberIdFieldName].Number();
        memberIds.insert(memberId);
        OpTime appliedOpTime;
        OpTime durableOpTime;
        bsonExtractOpTimeField(entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime)
            .transitional_ignore();
        ASSERT_EQUALS(optime, appliedOpTime);
        bsonExtractOpTimeField(entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime)
            .transitional_ignore();
        ASSERT_EQUALS(optime, durableOpTime);
    }
    ASSERT_EQUALS(2U, memberIds.size());

    // Advance the clock far enough to cause the other node to be marked as DOWN.
    const Date_t startDate = getNet()->now();
    const Date_t endDate = startDate + Milliseconds(2000);
    getNet()->enterNetwork();
    while (getNet()->now() < endDate) {
        getNet()->runUntil(endDate);
        if (getNet()->now() < endDate) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
    }
    getNet()->exitNetwork();

    // Check there is one entry in our UpdatePosition, since we shouldn't forward for a
    // DOWN node.
    BSONObj cmd2 = unittest::assertGet(getReplCoord()->prepareReplSetUpdatePositionCommand());
    std::set<long long> memberIds2;
    BSONForEach(entryElement, cmd2[UpdatePositionArgs::kUpdateArrayFieldName].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry[UpdatePositionArgs::kMemberIdFieldName].Number();
        memberIds2.insert(memberId);
        OpTime appliedOpTime;
        OpTime durableOpTime;
        bsonExtractOpTimeField(entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime)
            .transitional_ignore();
        ASSERT_EQUALS(optime, appliedOpTime);
        bsonExtractOpTimeField(entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime)
            .transitional_ignore();
        ASSERT_EQUALS(optime, durableOpTime);
    }
    ASSERT_EQUALS(1U, memberIds2.size());
}

TEST_F(ReplCoordTest, UpdatePositionCmdHasMetadata) {
    const auto replicaSetId = OID::gen();
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234")
                           << BSON("_id" << 2 << "host"
                                         << "test3:1234"))
             << "protocolVersion" << 1 << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000
                                             << "replicaSetId" << replicaSetId)),
        HostAndPort("test1", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    OpTime optime(Timestamp(100, 2), 0);
    replCoordSetMyLastAppliedOpTime(optime, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(optime, Date_t() + Seconds(100));
    auto opCtx = makeOperationContext();

    // Set last committed optime via metadata. Pass dummy Date_t to avoid advanceCommitPoint
    // invariant.
    rpc::ReplSetMetadata syncSourceMetadata(optime.getTerm(),
                                            {optime, Date_t() + Seconds(optime.getSecs())},
                                            optime,
                                            1,
                                            0,
                                            OID(),
                                            1,
                                            false);
    getReplCoord()->processReplSetMetadata(syncSourceMetadata);
    // Pass dummy Date_t to avoid advanceCommitPoint invariant.
    getReplCoord()->advanceCommitPoint({optime, Date_t() + Seconds(optime.getSecs())}, true);

    BSONObj cmd = unittest::assertGet(getReplCoord()->prepareReplSetUpdatePositionCommand());
    auto metadata = unittest::assertGet(rpc::ReplSetMetadata::readFromMetadata(cmd));
    ASSERT_EQUALS(metadata.getTerm(), getReplCoord()->getTerm());
    ASSERT_EQUALS(metadata.getLastOpVisible(), optime);
    ASSERT_EQUALS(metadata.getReplicaSetId(), replicaSetId);

    auto oqMetadataStatus = rpc::OplogQueryMetadata::readFromMetadata(cmd);
    ASSERT_EQUALS(oqMetadataStatus.getStatus(), ErrorCodes::NoSuchKey);
}

TEST_F(ReplCoordTest, StepDownWhenHandleLivenessTimeoutMarksAMajorityOfVotingNodesDown) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("host"
                                << "node1:12345"
                                << "_id" << 0)
                           << BSON("host"
                                   << "node2:12345"
                                   << "_id" << 1)
                           << BSON("host"
                                   << "node3:12345"
                                   << "_id" << 2)
                           << BSON("host"
                                   << "node4:12345"
                                   << "_id" << 3)
                           << BSON("host"
                                   << "node5:12345"
                                   << "_id" << 4))
             << "protocolVersion" << 1 << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime startingOpTime = OpTime(Timestamp(100, 1), 0);
    replCoordSetMyLastAppliedOpTime(startingOpTime, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(startingOpTime, Date_t() + Seconds(100));

    // Receive notification that every node is up.
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << 2 << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(startingOpTime.getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(startingOpTime.getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << 2 << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << 2 << UpdatePositionArgs::kMemberIdFieldName << 3
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << 2 << UpdatePositionArgs::kMemberIdFieldName << 4
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs()))))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args));
    // Become PRIMARY.
    simulateSuccessfulV1Election();

    // Keep two nodes alive via UpdatePosition.
    UpdatePositionArgs args1;
    ASSERT_OK(args1.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << 2 << UpdatePositionArgs::kMemberIdFieldName << 1
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName
                    << Date_t() + Seconds(startingOpTime.getSecs())
                    << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName
                    << Date_t() + Seconds(startingOpTime.getSecs()))
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << 2 << UpdatePositionArgs::kMemberIdFieldName << 2
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs())
                       << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName
                       << Date_t() + Seconds(startingOpTime.getSecs()))))));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    getNet()->runUntil(startDate + Milliseconds(100));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args1));

    // Confirm that the node remains PRIMARY after the other two nodes are marked DOWN.
    getNet()->runUntil(startDate + Milliseconds(2080));
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getReplCoord()->getMemberState().s);

    // Keep the same two nodes alive via v1 heartbeat.
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(2);
    hbArgs.setSenderId(1);
    hbArgs.setSenderHost(HostAndPort("node2", 12345));
    hbArgs.setTerm(0);
    ReplSetHeartbeatResponse hbResp;
    ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &hbResp));
    hbArgs.setSenderId(2);
    hbArgs.setSenderHost(HostAndPort("node3", 12345));
    ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &hbResp));

    // Confirm that the node remains PRIMARY after the timeout from the UpdatePosition expires.
    getNet()->enterNetwork();
    const Date_t endDate = startDate + Milliseconds(2200);
    while (getNet()->now() < endDate) {
        getNet()->runUntil(endDate);
        if (getNet()->now() < endDate) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
    }
    getNet()->exitNetwork();
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getReplCoord()->getMemberState().s);

    // Keep one node alive via two methods (UpdatePosition and requestHeartbeat).
    UpdatePositionArgs args2;
    ASSERT_OK(args2.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                           << 2 << UpdatePositionArgs::kMemberIdFieldName << 1
                           << UpdatePositionArgs::kDurableOpTimeFieldName << startingOpTime.toBSON()
                           << UpdatePositionArgs::kDurableWallTimeFieldName
                           << Date_t() + Seconds(startingOpTime.getSecs())
                           << UpdatePositionArgs::kAppliedOpTimeFieldName << startingOpTime.toBSON()
                           << UpdatePositionArgs::kAppliedWallTimeFieldName
                           << Date_t() + Seconds(startingOpTime.getSecs()))))));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args2));

    hbArgs.setSetName("mySet");
    hbArgs.setConfigVersion(2);
    hbArgs.setSenderId(1);
    hbArgs.setSenderHost(HostAndPort("node2", 12345));
    hbArgs.setTerm(0);
    ASSERT_OK(getReplCoord()->processHeartbeatV1(hbArgs, &hbResp));

    // Confirm that the node relinquishes PRIMARY after only one node is left UP.
    const Date_t startDate1 = getNet()->now();
    const Date_t endDate1 = startDate1 + Milliseconds(1980);
    getNet()->enterNetwork();
    while (getNet()->now() < endDate1) {
        getNet()->runUntil(endDate1);
        if (getNet()->now() < endDate1) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
    }
    getNet()->exitNetwork();

    ASSERT_OK(getReplCoord()->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_SECONDARY, Hours{1}));
}

TEST_F(ReplCoordTest, WaitForMemberState) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto replCoord = getReplCoord();
    auto initialTerm = replCoord->getTerm();
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(1, 1), 0), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(1, 1), 0), Date_t() + Seconds(100));
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    // Single node cluster - this node should start election on setFollowerMode() completion.
    replCoord->waitForElectionFinish_forTest();

    // Successful dry run election increases term.
    ASSERT_EQUALS(initialTerm + 1, replCoord->getTerm());

    auto timeout = Milliseconds(1);
    ASSERT_OK(replCoord->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_PRIMARY, timeout));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit,
                  replCoord->waitForMemberState(
                      Interruptible::notInterruptible(), MemberState::RS_REMOVED, timeout));

    ASSERT_EQUALS(ErrorCodes::BadValue,
                  replCoord->waitForMemberState(Interruptible::notInterruptible(),
                                                MemberState::RS_PRIMARY,
                                                Milliseconds(-1)));

    // Zero timeout is fine.
    ASSERT_OK(replCoord->waitForMemberState(
        Interruptible::notInterruptible(), MemberState::RS_PRIMARY, Milliseconds(0)));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit,
                  replCoord->waitForMemberState(
                      Interruptible::notInterruptible(), MemberState::RS_ARBITER, Milliseconds(0)));

    // Make sure it can be interrupted.
    auto opCtx = makeOperationContext();
    opCtx->markKilled(ErrorCodes::Interrupted);
    ASSERT_THROWS(
        replCoord->waitForMemberState(opCtx.get(), MemberState::RS_ARBITER, Milliseconds(0)),
        ExceptionFor<ErrorCodes::Interrupted>);
}

TEST_F(
    ReplCoordTest,
    PopulateUnsetWriteConcernOptionsSyncModeReturnsInputWithSyncModeNoneIfUnsetAndWriteConcernMajorityJournalDefaultIsFalse) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault" << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.w = WriteConcernOptions::kMajority;
    wc.syncMode = WriteConcernOptions::SyncMode::UNSET;
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(
    ReplCoordTest,
    PopulateUnsetWriteConcernOptionsSyncModeReturnsInputWithSyncModeJournalIfUnsetAndWriteConcernMajorityJournalDefaultIsTrue) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault" << true),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.w = WriteConcernOptions::kMajority;
    wc.syncMode = WriteConcernOptions::SyncMode::UNSET;
    ASSERT(WriteConcernOptions::SyncMode::JOURNAL ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(ReplCoordTest, PopulateUnsetWriteConcernOptionsSyncModeReturnsInputIfSyncModeIsNotUnset) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault" << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.w = WriteConcernOptions::kMajority;
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);

    wc.syncMode = WriteConcernOptions::SyncMode::JOURNAL;
    ASSERT(WriteConcernOptions::SyncMode::JOURNAL ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);

    wc.syncMode = WriteConcernOptions::SyncMode::FSYNC;
    ASSERT(WriteConcernOptions::SyncMode::FSYNC ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(ReplCoordTest, PopulateUnsetWriteConcernOptionsSyncModeReturnsInputIfWModeIsNotMajority) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault" << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.syncMode = WriteConcernOptions::SyncMode::UNSET;
    wc.w = "not the value of kMajority";
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);

    wc.w = "like literally anythingelse";
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(ReplCoordTest, NodeStoresElectionVotes) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                                         << "mySet"
                                                         << "term" << 7LL << "candidateIndex" << 2LL
                                                         << "configVersion" << 2LL << "dryRun"
                                                         << false << "lastAppliedOpTime"
                                                         << time.asOpTime().toBSON())));
    ReplSetRequestVotesResponse response;

    ASSERT_OK(getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response));
    ASSERT_EQUALS("", response.getReason());
    ASSERT_TRUE(response.getVoteGranted());

    auto lastVote = getExternalState()->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());

    // This is not a dry-run election so the last vote should include the new term and candidate.
    ASSERT_EQUALS(lastVote.getValue().getTerm(), 7);
    ASSERT_EQUALS(lastVote.getValue().getCandidateIndex(), 2);
}

TEST_F(ReplCoordTest, NodeDoesNotStoreDryRunVotes) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))),
                       HostAndPort("node1", 12345));
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                                         << "mySet"
                                                         << "term" << 7LL << "candidateIndex" << 2LL
                                                         << "configVersion" << 2LL << "dryRun"
                                                         << true << "lastAppliedOpTime"
                                                         << time.asOpTime().toBSON())));
    ReplSetRequestVotesResponse response;

    ASSERT_OK(getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response));
    ASSERT_EQUALS("", response.getReason());
    ASSERT_TRUE(response.getVoteGranted());

    auto lastVote = getExternalState()->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());

    // This is a dry-run election so the last vote should not be updated with the new term and
    // candidate.
    ASSERT_EQUALS(lastVote.getValue().getTerm(), 1);
    ASSERT_EQUALS(lastVote.getValue().getCandidateIndex(), 0);
}

TEST_F(ReplCoordTest, NodeFailsVoteRequestIfItFailsToStoreLastVote) {
    // Set up a 2-node replica set config.
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
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    // Get our current term, as primary.
    ASSERT(getReplCoord()->getMemberState().primary());
    auto initTerm = getReplCoord()->getTerm();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes"
                                   << 1 << "setName"
                                   << "mySet"
                                   << "term" << initTerm + 1  // term of new candidate.
                                   << "candidateIndex" << 1LL << "configVersion" << 2LL << "dryRun"
                                   << false << "lastAppliedOpTime" << time.asOpTime().toBSON())));
    ReplSetRequestVotesResponse response;

    // Simulate a failure to write the 'last vote' document. The specific error code isn't
    // important.
    getExternalState()->setStoreLocalLastVoteDocumentStatus(
        Status(ErrorCodes::OutOfDiskSpace, "failed to write last vote document"));

    // Make sure the vote request fails. If an error is returned, the filled out response is
    // invalid, so we do not check its contents.
    auto status = getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response);
    ASSERT_EQ(ErrorCodes::OutOfDiskSpace, status.code());

    auto lastVote = unittest::assertGet(getExternalState()->loadLocalLastVoteDocument(opCtx.get()));

    // The last vote doc should store the vote of the first election, not the one we failed to cast
    // our vote in.
    ASSERT_EQUALS(lastVote.getTerm(), initTerm);
    ASSERT_EQUALS(lastVote.getCandidateIndex(), 0);
}

TEST_F(ReplCoordTest, NodeDoesNotGrantVoteIfInTerminalShutdown) {
    // Set up a 2-node replica set config.
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
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    // Get our current term, as primary.
    ASSERT(getReplCoord()->getMemberState().primary());
    auto initTerm = getReplCoord()->getTerm();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes"
                                   << 1 << "setName"
                                   << "mySet"
                                   << "term" << initTerm + 1  // term of new candidate.
                                   << "candidateIndex" << 1LL << "configVersion" << 2LL << "dryRun"
                                   << false << "lastAppliedOpTime" << time.asOpTime().toBSON())));
    ReplSetRequestVotesResponse response;

    getReplCoord()->enterTerminalShutdown();

    auto r = getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response);

    ASSERT_NOT_OK(r);
    ASSERT_EQUALS("In the process of shutting down", r.reason());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, r.code());
}

TEST_F(ReplCoordTest, RemovedNodeDoesNotGrantVote) {
    // A 1-node set. This node is not a member.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0))),
                       HostAndPort("node2", 12345));

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_REMOVED));
    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON(
        "replSetRequestVotes" << 1 << "setName"
                              << "mySet"
                              << "term" << 2 << "candidateIndex" << 0LL << "configVersion" << 1LL
                              << "dryRun" << false << "lastAppliedOpTime" << OpTime().toBSON())));

    ReplSetRequestVotesResponse response;
    auto opCtx = makeOperationContext();
    auto r = getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response);
    ASSERT_NOT_OK(r);
    ASSERT_EQUALS("Invalid replica set config, or this node is not a member", r.reason());
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, r.code());

    // Vote was not recorded.
    ServiceContext* svcCtx = getServiceContext();
    ASSERT(ReplicationMetrics::get(svcCtx).getElectionParticipantMetricsBSON().isEmpty());
}

TEST_F(ReplCoordTest, CheckIfCommitQuorumHasReached) {
    // Set up a 5-node replica set config.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node0:100"
                                                     << "tags"
                                                     << BSON("dc"
                                                             << "NA"
                                                             << "rack"
                                                             << "rackNA1"))
                                          << BSON("_id" << 1 << "host"
                                                        << "node1:101"
                                                        << "tags"
                                                        << BSON("dc"
                                                                << "NA"
                                                                << "rack"
                                                                << "rackNA2"))
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:102"
                                                        << "tags"
                                                        << BSON("dc"
                                                                << "NA"
                                                                << "rack"
                                                                << "rackNA3"))
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:103"
                                                        << "tags"
                                                        << BSON("dc"
                                                                << "EU"
                                                                << "rack"
                                                                << "rackEU1"))
                                          << BSON("_id" << 4 << "host"
                                                        << "node4:104"
                                                        << "votes" << 0 << "priority" << 0 << "tags"
                                                        << BSON("dc"
                                                                << "EU"
                                                                << "rack"
                                                                << "rackEU2"))
                                          << BSON("_id" << 5 << "host"
                                                        << "node5:105"
                                                        << "arbiterOnly" << true))
                            << "settings"
                            << BSON("getLastErrorModes" << BSON(
                                        "valid" << BSON("dc" << 2 << "rack" << 2)
                                                << "invalidNotEnoughValues" << BSON("dc" << 3)
                                                << "invalidNotEnoughNodes" << BSON("rack" << 6)))),
                       HostAndPort("node0", 100));

    auto replCoord = getReplCoord();
    {
        // Quorum contains a node which is not part of the replica set config.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node100", 100)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains two nodes from same data center.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node0", 100),
                                                    HostAndPort("node1", 101)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains two nodes from different data center.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node1", 101),
                                                    HostAndPort("node3", 103)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains majority of voting data bearing nodes.
        std::vector<HostAndPort> commitReadyMembers{
            HostAndPort("node1", 101), HostAndPort("node2", 102), HostAndPort("node3", 103)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains all voting data bearing nodes.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node0", 100),
                                                    HostAndPort("node1", 101),
                                                    HostAndPort("node2", 102),
                                                    HostAndPort("node3", 103)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains all data bearing nodes.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node0", 100),
                                                    HostAndPort("node1", 101),
                                                    HostAndPort("node2", 102),
                                                    HostAndPort("node3", 103),
                                                    HostAndPort("node4", 104)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions allNodesCQ;
        allNodesCQ.numNodes = 5;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(allNodesCQ, commitReadyMembers));
    }

    {
        // Quorum contains arbiter.
        std::vector<HostAndPort> commitReadyMembers{HostAndPort("node1", 101),
                                                    HostAndPort("node2", 102),
                                                    HostAndPort("node3", 103),
                                                    HostAndPort("node5", 105)};

        CommitQuorumOptions singleNodeCQ;
        singleNodeCQ.numNodes = 1;
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(singleNodeCQ, commitReadyMembers));

        CommitQuorumOptions vaildModeCQ;
        vaildModeCQ.mode = "valid";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(vaildModeCQ, commitReadyMembers));

        CommitQuorumOptions majorityModeCQ;
        majorityModeCQ.mode = "majority";
        ASSERT_TRUE(replCoord->isCommitQuorumSatisfied(majorityModeCQ, commitReadyMembers));

        CommitQuorumOptions votingMembersModeCQ;
        votingMembersModeCQ.mode = "votingMembers";
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(votingMembersModeCQ, commitReadyMembers));

        CommitQuorumOptions numNodesCQ;
        numNodesCQ.numNodes = 4;
        ASSERT_FALSE(replCoord->isCommitQuorumSatisfied(numNodesCQ, commitReadyMembers));
    }
}

TEST_F(ReplCoordTest, NodeFailsVoteRequestIfCandidateIndexIsInvalid) {
    // Set up a 2-node replica set config.
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
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(time, Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(time, Date_t() + Seconds(100));
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    // Invalid candidateIndex values.
    for (auto candidateIndex : std::vector<long long>{-1LL, 2LL}) {
        ReplSetRequestVotesArgs args;
        ASSERT_OK(args.initialize(BSON(
            "replSetRequestVotes" << 1 << "setName"
                                  << "mySet"
                                  << "term" << getReplCoord()->getTerm() << "candidateIndex"
                                  << candidateIndex << "configVersion" << 2LL << "dryRun" << false
                                  << "lastAppliedOpTime" << time.asOpTime().toBSON())));
        ReplSetRequestVotesResponse response;
        auto r = getReplCoord()->processReplSetRequestVotes(opCtx.get(), args, &response);

        ASSERT_NOT_OK(r);
        ASSERT_STRING_CONTAINS(r.reason(), "Invalid candidateIndex");
        ASSERT_EQUALS(ErrorCodes::BadValue, r.code());
    }
}

TEST_F(ReplCoordTest, ShouldChooseNearestNodeAsSyncSourceWhenSecondaryAndChainingAllowed) {
    // Set up a three-node replica set with chainingAllowed set to true.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "settings" << BSON("chainingAllowed" << true)),
                       HostAndPort("node1", 12345));

    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ReplSetHeartbeatResponse hbResp;
    hbResp.setTerm(1);
    hbResp.setConfigVersion(2);
    hbResp.setConfigTerm(1);
    hbResp.setSetName("mySet");

    const OpTime lastAppliedOpTime = OpTime(Timestamp(50, 0), 1);
    const auto now = getNet()->now();
    hbResp.setAppliedOpTimeAndWallTime({lastAppliedOpTime, now});
    hbResp.setDurableOpTimeAndWallTime({lastAppliedOpTime, now});

    // Set the primary's ping to be longer than the other secondary's ping.
    const auto primaryPing = Milliseconds(10);
    const auto nearestNodePing = Milliseconds(5);

    // We must send two heartbeats per node, so that we satisfy the 2N requirement before choosing a
    // new sync source.
    for (auto i = 0; i < 2; i++) {
        hbResp.setState(MemberState::RS_PRIMARY);
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 1 /* targetIndex */, primaryPing);
        hbResp.setState(MemberState::RS_SECONDARY);
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 2 /* targetIndex */, nearestNodePing);
    }

    // We expect to sync from the closest node, since our read preference should be set to
    // ReadPreference::Nearest.
    ASSERT_EQ(HostAndPort("node3:12345"), replCoord->chooseNewSyncSource(OpTime()));
}

TEST_F(ReplCoordTest, ShouldChoosePrimaryAsSyncSourceWhenSecondaryAndChainingNotAllowed) {
    // Set up a three-node replica set with chainingAllowed set to false.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "settings" << BSON("chainingAllowed" << false)),
                       HostAndPort("node1", 12345));

    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    ReplSetHeartbeatResponse hbResp;
    hbResp.setTerm(1);
    hbResp.setConfigVersion(2);
    hbResp.setConfigTerm(1);
    hbResp.setSetName("mySet");

    const OpTime lastAppliedOpTime = OpTime(Timestamp(50, 0), 1);
    const auto now = getNet()->now();
    hbResp.setAppliedOpTimeAndWallTime({lastAppliedOpTime, now});
    hbResp.setDurableOpTimeAndWallTime({lastAppliedOpTime, now});

    // Set the primary's ping to be longer than the other secondary's ping.
    const auto primaryPing = Milliseconds(10);
    const auto nearestNodePing = Milliseconds(5);

    // We must send two heartbeats per node, so that we satisfy the 2N requirement before choosing a
    // new sync source.
    for (auto i = 0; i < 2; i++) {
        hbResp.setState(MemberState::RS_PRIMARY);
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 1 /* targetIndex */, primaryPing);
        hbResp.setState(MemberState::RS_SECONDARY);
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 2 /* targetIndex */, nearestNodePing);
    }

    // We expect to sync from the primary even though it is farther away, since our read preference
    // should be set to ReadPreference::PrimaryOnly.
    ASSERT_EQ(HostAndPort("node2:12345"), replCoord->chooseNewSyncSource(OpTime()));
}

TEST_F(ReplCoordTest, ShouldChooseNearestNodeAsSyncSourceWhenPrimaryAndChainingAllowed) {
    // Set up a three-node replica set with chainingAllowed set to true.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id" << 2))
                            << "settings" << BSON("chainingAllowed" << true)),
                       HostAndPort("node1", 12345));

    const auto opCtx = makeOperationContext();

    // Get elected primary.
    auto replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
    replCoordSetMyLastAppliedOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    replCoordSetMyLastDurableOpTime(OpTimeWithTermOne(100, 1), Date_t() + Seconds(100));
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(
        getReplCoord()->getElectionTimeout_forTest(), opCtx.get());
    ASSERT(replCoord->getMemberState().primary());
    ASSERT(replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Draining);

    ReplSetHeartbeatResponse hbResp;
    hbResp.setTerm(1);
    hbResp.setConfigVersion(2);
    hbResp.setConfigTerm(1);
    hbResp.setSetName("mySet");
    const OpTime lastAppliedOpTime = OpTime(Timestamp(50, 0), 1);
    const auto now = getNet()->now();
    hbResp.setAppliedOpTimeAndWallTime({lastAppliedOpTime, now});
    hbResp.setDurableOpTimeAndWallTime({lastAppliedOpTime, now});
    hbResp.setState(MemberState::RS_SECONDARY);

    const auto furthestNodePing = Milliseconds(10);
    const auto nearestNodePing = Milliseconds(5);

    // We must send two heartbeats per node, so that we satisfy the 2N requirement before choosing a
    // new sync source.
    for (auto i = 0; i < 2; i++) {
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 1 /* targetIndex */, furthestNodePing);
        replCoord->handleHeartbeatResponse_forTest(
            hbResp.toBSON(), 2 /* targetIndex */, nearestNodePing);
    }

    // We expect to sync from the closest node, since our read preference should be set to
    // ReadPreference::Nearest.
    ASSERT_EQ(HostAndPort("node3:12345"), replCoord->chooseNewSyncSource(OpTime()));
}

TEST_F(ReplCoordTest, IgnoreNonNullDurableOpTimeOrWallTimeForArbiterFromReplSetUpdatePosition) {
    init("mySet/node1:12345,node2:12345,node3:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345")
                                          << BSON("_id" << 3 << "host"
                                                        << "node3:12345"
                                                        << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    const auto repl = getReplCoord();

    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 2);
    Date_t wallTime1 = Date_t() + Seconds(10);
    Date_t wallTime2 = Date_t() + Seconds(20);

    // Node 1 is ahead, nodes 2 and 3 a bit behind.
    // Node 3 should not have a durable optime/walltime as they are an arbiter.
    repl->setMyLastAppliedOpTimeAndWallTime({opTime2, wallTime2});
    repl->setMyLastDurableOpTimeAndWallTime({opTime2, wallTime2});
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1, wallTime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 3, opTime1, wallTime1));
    ASSERT_OK(repl->setLastDurableOptime_forTest(1, 2, opTime1, wallTime1));
    ASSERT_OK(repl->setLastDurableOptime_forTest(1, 3, OpTime(), Date_t()));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    // Receive updates on behalf of nodes 2 and 3 from node 2.
    // Node 3 will be reported as caught up both in lastApplied and lastDurable, but we
    // must ignore the lastDurable part as null is the only valid value for arbiters.
    UpdatePositionArgs updatePositionArgs;

    ASSERT_OK(updatePositionArgs.initialize(BSON(
        UpdatePositionArgs::kCommandFieldName
        << 1 << UpdatePositionArgs::kUpdateArrayFieldName
        << BSON_ARRAY(
               BSON(UpdatePositionArgs::kConfigVersionFieldName
                    << repl->getConfig().getConfigVersion()
                    << UpdatePositionArgs::kMemberIdFieldName << 2
                    << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kAppliedWallTimeFieldName << wallTime2
                    << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                    << UpdatePositionArgs::kDurableWallTimeFieldName << wallTime2)
               << BSON(UpdatePositionArgs::kConfigVersionFieldName
                       << repl->getConfig().getConfigVersion()
                       << UpdatePositionArgs::kMemberIdFieldName << 3
                       << UpdatePositionArgs::kAppliedOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kAppliedWallTimeFieldName << wallTime2
                       << UpdatePositionArgs::kDurableOpTimeFieldName << opTime2.asOpTime().toBSON()
                       << UpdatePositionArgs::kDurableWallTimeFieldName << wallTime2)))));

    startCapturingLogMessages();
    ASSERT_OK(repl->processReplSetUpdatePosition(updatePositionArgs));

    // Make sure node 2 is fully caught up but node 3 has null durable optime/walltime.
    auto memberDataVector = repl->getMemberData();
    for (auto member : memberDataVector) {
        auto memberId = member.getMemberId();
        if (memberId == MemberId(1) || memberId == MemberId(2)) {
            ASSERT_EQ(member.getLastAppliedOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastAppliedWallTime(), wallTime2);
            ASSERT_EQ(member.getLastDurableOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastDurableWallTime(), wallTime2);
            continue;
        } else if (member.getMemberId() == MemberId(3)) {
            ASSERT_EQ(member.getLastAppliedOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastAppliedWallTime(), wallTime2);
            ASSERT_EQ(member.getLastDurableOpTime(), OpTime());
            ASSERT_EQ(member.getLastDurableWallTime(), Date_t());
            continue;
        }
        MONGO_UNREACHABLE;
    }
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1,
        countTextFormatLogLinesContaining(
            "Received non-null durable optime/walltime for arbiter from replSetUpdatePosition"));
}

TEST_F(ReplCoordTest, IgnoreNonNullDurableOpTimeOrWallTimeForArbiterFromHeartbeat) {
    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kReplication,
                                                       logv2::LogSeverity::Debug(1)};
    init("mySet/node1:12345,node2:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:12345"
                                                        << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    const auto repl = getReplCoord();

    OpTimeWithTermOne opTime1(100, 1);
    OpTimeWithTermOne opTime2(200, 2);
    Date_t wallTime1 = Date_t() + Seconds(10);
    Date_t wallTime2 = Date_t() + Seconds(20);

    // Node 1 is ahead, nodes 2 is a bit behind.
    // Node 2 should not have a durable optime/walltime as they are an arbiter.
    repl->setMyLastAppliedOpTimeAndWallTime({opTime2, wallTime2});
    repl->setMyLastDurableOpTimeAndWallTime({opTime2, wallTime2});
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, opTime1, wallTime1));
    ASSERT_OK(repl->setLastDurableOptime_forTest(1, 2, OpTime(), Date_t()));

    simulateSuccessfulV1Election();
    ASSERT_TRUE(repl->getMemberState().primary());

    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setTerm(1);
    hbResp.setConfigVersion(2);
    hbResp.setConfigTerm(1);
    hbResp.setState(MemberState::RS_ARBITER);
    hbResp.setAppliedOpTimeAndWallTime({opTime2, wallTime2});
    hbResp.setDurableOpTimeAndWallTime({opTime2, wallTime2});

    startCapturingLogMessages();
    repl->handleHeartbeatResponse_forTest(
        hbResp.toBSON(), 1 /* targetIndex */, Milliseconds(5) /* ping */);

    auto memberDataVector = repl->getMemberData();
    for (auto member : memberDataVector) {
        auto memberId = member.getMemberId();
        if (memberId == MemberId(1)) {
            ASSERT_EQ(member.getLastAppliedOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastAppliedWallTime(), wallTime2);
            ASSERT_EQ(member.getLastDurableOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastDurableWallTime(), wallTime2);
            continue;
        } else if (member.getMemberId() == MemberId(2)) {
            ASSERT_EQ(member.getLastAppliedOpTime(), opTime2.asOpTime());
            ASSERT_EQ(member.getLastAppliedWallTime(), wallTime2);
            ASSERT_EQ(member.getLastDurableOpTime(), OpTime());
            ASSERT_EQ(member.getLastDurableWallTime(), Date_t());
            continue;
        }
        MONGO_UNREACHABLE;
    }

    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countTextFormatLogLinesContaining(
                      "Received non-null durable optime/walltime for arbiter from heartbeat"));
}

// TODO(schwerin): Unit test election id updating
}  // namespace
}  // namespace repl
}  // namespace mongo
