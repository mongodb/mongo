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

#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

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

/**
 * Helper that kills an operation, taking the necessary locks.
 */
void killOperation(OperationContext* opCtx) {
    stdx::lock_guard<Client> lkClient(*opCtx->getClient());
    opCtx->getServiceContext()->killOperation(opCtx);
}

TEST_F(ReplCoordTest, NodeEntersStartup2StateWhenStartingUpWithValidLocalConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    ASSERT_TRUE(getExternalState()->threadsStarted());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeEntersArbiterStateWhenStartingUpWithValidLocalConfigWhereItIsAnArbiter) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"
                                                     << "arbiterOnly"
                                                     << true)
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345")
                                          << BSON("_id" << 2 << "host"
                                                        << "node2:54321"))),
                       HostAndPort("node3", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("NodeNotFound"));
    ASSERT_EQUALS(MemberState::RS_REMOVED, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest,
       NodeEntersRemovedStateWhenStartingUpWithALocalConfigContainingTheWrongSetName) {
    init("mySet");
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "notMySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("reports set name of notMySet,"));
    ASSERT_EQUALS(MemberState::RS_REMOVED, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, NodeEntersStartupStateWhenStartingUpWithNoLocalConfig) {
    startCapturingLogMessages();
    start();
    stopCapturingLogMessages();
    ASSERT_EQUALS(3, countLogLinesContaining("Did not find local "));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                                                    << "version"
                                                    << 1
                                                    << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"
                                                                             << "arbiterOnly"
                                                                             << true)
                                                                  << BSON("_id" << 1 << "host"
                                                                                << "node2:12345"))),
                                               &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "is not electable under the new configuration version");
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                                               << "version"
                                               << 1
                                               << "members"
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
    hbArgs.setCheckEmpty();
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);
    hbArgs.setTerm(0);
    hbArgs.setHeartbeatVersion(1);

    auto appliedTS = Timestamp(3, 3);
    getReplCoord()->setMyLastAppliedOpTime(OpTime(appliedTS, 1));

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread([&] { doReplSetInitiate(getReplCoord(), &status); });
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_BSONOBJ_EQ(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(0);
    getNet()->scheduleResponse(
        noi,
        startDate + Milliseconds(10),
        RemoteCommandResponse(hbResp.toBSON(false), BSONObj(), Milliseconds(8)));
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
    ASSERT_STRING_CONTAINS(status.reason(), "Missing expected field \"_id\"");
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
        BSON("version" << 1 << "members" << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                  << "node1:12345"))),
        &result1);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Missing expected field \"_id\"");
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
                                                    << "version"
                                                    << 2
                                                    << "members"
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                                                    << "version"
                                                    << 1
                                                    << "members"
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
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
    writeConcern.wNumNodes = 2;

    // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
    // awaitReplication will always work.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, NodeReturnsNotMasterWhenRunningAwaitReplicationAgainstASecondaryNode) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();

    OpTimeWithTermOne time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 0;  // Waiting for 0 nodes always works
    writeConcern.wMode = "";

    // Node should fail to awaitReplication when not primary.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern);
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, statusAndDur.status);
}

TEST_F(ReplCoordTest, NodeReturnsOkWhenRunningAwaitReplicationAgainstPrimaryWithWTermOne) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 0;  // Waiting for 0 nodes always works
    writeConcern.wMode = "";

    // Become primary.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id"
                                                  << 3))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 2);
    OpTimeWithTermOne time2(100, 3);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;

    auto opCtx = makeOperationContext();
    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.wNumNodes = 2;
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
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id"
                                                  << 3))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 2);
    OpTimeWithTermOne time2(100, 3);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    auto opCtx = makeOperationContext();


    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.wNumNodes = 2;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time2
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    ASSERT_OK(getReplCoord()->setLastDurableOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 3, time2));
    statusAndDur = getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest,
       NodeReturnsUnknownReplWriteConcernWhenAwaitReplicationReceivesAnInvalidWriteConcernMode) {
    auto service = stdx::make_unique<ServiceContextNoop>();
    auto client = service->makeClient("test");
    auto opCtx = client->makeOperationContext();

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
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
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    // Test invalid write concern
    WriteConcernOptions invalidWriteConcern;
    invalidWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    invalidWriteConcern.wMode = "fakemode";

    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(opCtx.get(), time1, invalidWriteConcern);
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern, statusAndDur.status);
}

TEST_F(
    ReplCoordTest,
    NodeReturnsWriteConcernFailedUntilASufficientSetOfNodesHaveTheWriteAndTheWriteIsInACommittedSnapshot) {
    auto service = stdx::make_unique<ServiceContextNoop>();
    auto client = service->makeClient("test");
    auto opCtx = client->makeOperationContext();

    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version"
             << 2
             << "members"
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
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    simulateSuccessfulV1Election();

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    // Set up valid write concerns for the rest of the test
    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;
    majorityWriteConcern.syncMode = WriteConcernOptions::SyncMode::JOURNAL;

    WriteConcernOptions multiDCWriteConcern;
    multiDCWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiDCWriteConcern.wMode = "multiDC";

    WriteConcernOptions multiRackWriteConcern;
    multiRackWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiRackWriteConcern.wMode = "multiDCAndRack";


    // Nothing satisfied
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
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
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time1
    awaiter.setOpTime(time1);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.start();
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, NodeReturnsWriteConcernFailedWhenAWriteConcernTimesOutBeforeBeingSatisified) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wDeadline = getNet()->now() + Milliseconds(50);
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

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

TEST_F(ReplCoordTest, NodeReturnsNotMasterWhenSteppingDownBeforeSatisfyingAWriteConcern) {
    // Test that a thread blocked in awaitReplication will be woken up and return NotMaster
    // if the node steps down while it is waiting.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    const auto opCtx = makeOperationContext();
    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 2, time1));
    ASSERT_OK(getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000)));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::PrimarySteppedDown, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsInterruptedWhenAnOpWaitingForWriteConcernToBeSatisfiedIsInterrupted) {
    // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node1")
                                          << BSON("_id" << 1 << "host"
                                                        << "node2")
                                          << BSON("_id" << 2 << "host"
                                                        << "node3"))),
                       HostAndPort("node1"));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;


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
                return getReplCoord()->stepDown(result.opCtx.get(), force, waitTime, stepDownTime);
            });
        auto result = task.get_future();
        PromisedClientAndOperation operationPromise;
        auto operationFuture = operationPromise.get_future();
        stdx::thread(std::move(task), std::move(operationPromise)).detach();

        getReplCoord()->waitForStepDownAttempt_forTest();

        return std::make_pair(operationFuture.get(), std::move(result));
    }

    // Makes it so enough secondaries are caught up that a stepdown command can succeed.
    void catchUpSecondaries(const OpTime& desiredOpTime) {
        auto config = getReplCoord()->getConfig();
        auto heartbeatInterval = config.getHeartbeatInterval();

        enterNetwork();
        getNet()->runUntil(getNet()->now() + heartbeatInterval);
        NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        RemoteCommandRequest request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(hbArgs.getSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(hbArgs.getConfigVersion());
            hbResp.setAppliedOpTime(desiredOpTime);
            hbResp.setDurableOpTime(desiredOpTime);
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj, false);
            getNet()->scheduleResponse(noi, getNet()->now(), makeResponseStatus(respObj.obj()));
        }
        while (getNet()->hasReadyRequests()) {
            auto noi = getNet()->getNextReadyRequest();
            log() << "Blackholing network request " << noi->getRequest().cmdObj;
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
                                << "version"
                                << 1
                                << "members"
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

TEST_F(ReplCoordTest, ElectionIdTracksTermInPV1) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("test1", 1234));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
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
    auto status = getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));

    ASSERT_OK(status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1ElectionWithoutExitingDrainMode(
        getReplCoord()->getElectionTimeout_forTest());

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
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("test1", 1234));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
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

    // higher term, step down and change term
    executor::TaskExecutor::CallbackHandle cbHandle;
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(opCtx.get(), 2).code());
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, ConcurrentStepDownShouldNotSignalTheSameFinishEventMoreThanOnce) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("test1", 1234));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1Election();

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    auto replExec = getReplExec();

    // Prevent _stepDownFinish() from running and becoming secondary by blocking in this
    // exclusive task.
    const auto opCtx = makeOperationContext();
    boost::optional<Lock::GlobalWrite> globalExclusiveLock(opCtx.get());

    TopologyCoordinator::UpdateTermResult termUpdated2;
    auto updateTermEvh2 = getReplCoord()->updateTerm_forTest(2, &termUpdated2);
    ASSERT(termUpdated2 == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
    ASSERT(updateTermEvh2.isValid());

    TopologyCoordinator::UpdateTermResult termUpdated3;
    auto updateTermEvh3 = getReplCoord()->updateTerm_forTest(3, &termUpdated3);
    ASSERT(termUpdated3 == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);
    // Although term 3 can trigger stepdown, a stepdown has already been scheduled,
    // so no other stepdown can be scheduled again. Term 3 will be remembered and
    // installed once stepdown finishes.
    ASSERT(!updateTermEvh3.isValid());

    // Unblock the tasks for updateTerm and _stepDownFinish.
    globalExclusiveLock.reset();

    // Wait stepdown to finish and term 3 to be installed.
    replExec->waitForEvent(updateTermEvh2);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
}

TEST_F(ReplCoordTest, DrainCompletionMidStepDown) {
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("test1", 1234));
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 0));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1ElectionWithoutExitingDrainMode(
        getReplCoord()->getElectionTimeout_forTest());

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now update term to trigger a stepdown.
    TopologyCoordinator::UpdateTermResult termUpdated;
    auto updateTermEvh = getReplCoord()->updateTerm_forTest(2, &termUpdated);
    ASSERT(updateTermEvh.isValid());
    ASSERT(termUpdated == TopologyCoordinator::UpdateTermResult::kTriggerStepDown);

    // Now signal that replication applier is finished draining its buffer.
    const auto opCtx = makeOperationContext();
    getReplCoord()->signalDrainComplete(opCtx.get(), getReplCoord()->getTerm());

    // Now wait for stepdown to complete
    getReplExec()->waitForEvent(updateTermEvh);

    // By now drain mode should be cancelled.
    ASSERT_OK(getReplCoord()->waitForDrainFinish(Milliseconds(0)));

    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    // ASSERT_EQUALS(2, getReplCoord()->getTerm()); // SERVER-28290
}

TEST_F(StepDownTest, NodeReturnsNotMasterWhenAskedToStepDownAsANonPrimaryNode) {
    const auto opCtx = makeOperationContext();

    OpTimeWithTermOne optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastAppliedOpTime(optime1);
    getReplCoord()->setMyLastDurableOpTime(optime1);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    Status status = getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(0));
    ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeReturnsExceededTimeLimitWhenStepDownFailsToObtainTheGlobalLockWithinTheAllottedTime) {
    OpTimeWithTermOne optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastAppliedOpTime(optime1);
    getReplCoord()->setMyLastDurableOpTime(optime1);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    const auto opCtx = makeOperationContext();

    // Make sure stepDown cannot grab the global shared lock
    Lock::GlobalWrite lk(opCtx.get());

    Status status =
        getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
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
                                    bool caughtUpAreElectable) {
        int hbNum = 1;
        while (getNet()->hasReadyRequests()) {
            NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
            RemoteCommandRequest request = noi->getRequest();

            // Only process heartbeat requests.
            ASSERT_EQ(request.cmdObj.firstElement().fieldNameStringData().toString(),
                      "replSetHeartbeat");

            ReplSetHeartbeatArgsV1 hbArgs;
            ASSERT_OK(hbArgs.initialize(request.cmdObj));

            log() << request.target.toString() << " processing " << request.cmdObj;

            // Catch up 'numNodesCaughtUp' nodes out of 5.
            OpTime optimeResponse = (hbNum <= numNodesCaughtUp) ? optimePrimary : optimeLagged;
            bool isElectable = (hbNum <= numNodesCaughtUp) ? caughtUpAreElectable : true;

            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(hbArgs.getSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(hbArgs.getConfigVersion());
            hbResp.setDurableOpTime(optimeResponse);
            hbResp.setAppliedOpTime(optimeResponse);
            hbResp.setElectable(isElectable);
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj, false);
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
                                << "version"
                                << 1
                                << "members"
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

TEST_F(
    StepDownTestFiveNode,
    NodeReturnsExceededTimeLimitWhenStepDownIsRunAndCaughtUpMajorityExistsButWithoutElectableNode) {
    OpTime optimeLagged(Timestamp(100, 1), 1);
    OpTime optimePrimary(Timestamp(100, 2), 1);

    // All nodes are caught up
    getReplCoord()->setMyLastAppliedOpTime(optimePrimary);
    getReplCoord()->setMyLastDurableOpTime(optimePrimary);
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
    simulateHeartbeatResponses(optimePrimary, optimeLagged, numNodesCaughtUp, false);
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    const auto opCtx = makeOperationContext();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    auto status = getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(StepDownTestFiveNode,
       NodeReturnsExceededTimeLimitWhenStepDownIsRunAndNoCaughtUpMajorityExists) {
    OpTime optimeLagged(Timestamp(100, 1), 1);
    OpTime optimePrimary(Timestamp(100, 2), 1);

    // All nodes are caught up
    getReplCoord()->setMyLastAppliedOpTime(optimePrimary);
    getReplCoord()->setMyLastDurableOpTime(optimePrimary);
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
    simulateHeartbeatResponses(optimePrimary, optimeLagged, numNodesCaughtUp, true);
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    const auto opCtx = makeOperationContext();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    auto status = getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(
    StepDownTestFiveNode,
    NodeTransitionsToSecondaryImmediatelyWhenStepDownIsRunAndAnUpToDateMajorityWithElectableNodeExists) {
    OpTime optimeLagged(Timestamp(100, 1), 1);
    OpTime optimePrimary(Timestamp(100, 2), 1);

    // All nodes are caught up
    getReplCoord()->setMyLastAppliedOpTime(optimePrimary);
    getReplCoord()->setMyLastDurableOpTime(optimePrimary);
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
    simulateHeartbeatResponses(optimePrimary, optimeLagged, numNodesCaughtUp, true);
    getNet()->runReadyNetworkOperations();
    exitNetwork();

    const auto opCtx = makeOperationContext();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT_OK(getReplCoord()->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000)));
    enterNetwork();  // So we can safely inspect the topology coordinator
    ASSERT_EQUALS(getNet()->now() + Seconds(1), getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, NodeBecomesPrimaryAgainWhenStepDownTimeoutExpiresInASingleNodeSet) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    ASSERT_OK(getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000)));
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
}

TEST_F(
    ReplCoordTest,
    NodeGoesIntoRecoveryAgainWhenStepDownTimeoutExpiresInASingleNodeSetAndWeAreInMaintenanceMode) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    const auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    ASSERT_OK(getReplCoord()->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000)));
    getNet()->enterNetwork();  // Must do this before inspecting the topocoord
    Date_t stepdownUntil = getNet()->now() + Seconds(1);
    ASSERT_EQUALS(stepdownUntil, getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Go into maintenance mode.
    ASSERT_EQUALS(0, getTopoCoord().getMaintenanceCount());
    ASSERT_FALSE(getReplCoord()->getMaintenanceMode());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
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
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    const auto opCtx = makeOperationContext();

    // Try to stepDown but time out because no secondaries are caught up.
    auto status = repl->stepDown(opCtx.get(), false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
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
    status = repl->stepDown(opCtx.get(), true, Milliseconds(0), Milliseconds(1000));
    ASSERT_OK(status);
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeTransitionsToSecondaryWhenASecondaryCatchesUpAfterTheFirstRoundOfHeartbeats) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    catchUpSecondaries(optime2);

    ASSERT_OK(*result.second.get());
    ASSERT_TRUE(repl->getMemberState().secondary());
}

TEST_F(StepDownTest,
       NodeTransitionsToSecondaryWhenASecondaryCatchesUpDuringStepDownsSecondaryCatchupPeriod) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // Secondary has not caught up on first round of heartbeats.
    enterNetwork();
    getNet()->runUntil(getNet()->now() + Milliseconds(1000));
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << "HB1: " << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgsV1 hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj, false);
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
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
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
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now while the first stepdown request is waiting for secondaries to catch up, attempt
    // another stepdown request and ensure it fails.
    const auto opCtx = makeOperationContext();
    auto status = getReplCoord()->stepDown(opCtx.get(), false, Seconds(10), Seconds(60));
    ASSERT_EQUALS(ErrorCodes::ConflictingOperationInProgress, status);

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
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.

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
// unconditional stepdown happens, and then is interrupted, we stay stepped down, even though
// normally if we were just interrupted we would step back up.
TEST_F(StepDownTest, InterruptingStepDownCommandRestoresWriteAvailability) {
    OpTime optime1(Timestamp(100, 1), 1);
    OpTime optime2(Timestamp(100, 2), 1);

    // No secondary is caught up
    auto repl = getReplCoord();
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.

    // Start a stepdown command that needs to wait for secondaries to catch up.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Interrupt the ongoing stepdown command.
    {
        stdx::lock_guard<Client> lk(*result.first.client.get());
        result.first.opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // Ensure that the stepdown command failed.
    ASSERT_EQUALS(*result.second.get(), ErrorCodes::Interrupted);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // This is the important check, that we didn't accidentally step back up when aborting the
    // stepdown command attempt.
    const auto opCtx = makeOperationContext();
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
    repl->setMyLastAppliedOpTime(optime2);
    repl->setMyLastDurableOpTime(optime2);
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 1, optime1));
    ASSERT_OK(repl->setLastAppliedOptime_forTest(1, 2, optime1));

    simulateSuccessfulV1Election();

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Step down where the secondary actually has to catch up before the stepDown can succeed.
    // On entering the network, _stepDownContinue should cancel the heartbeats scheduled for
    // T + 2 seconds and send out a new round of heartbeats immediately.
    // This makes it unnecessary to advance the clock after entering the network to process
    // the heartbeat requests.

    // Start a stepdown command that needs to wait for secondaries to catch up.
    auto result = stepDown_nonBlocking(false, Seconds(10), Seconds(60));

    // We should still be primary at this point
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Interrupt the ongoing stepdown command.
    {
        stdx::lock_guard<Client> lk(*result.first.client.get());
        result.first.opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // Now while the first stepdown request is waiting for secondaries to catch up, force an
    // unconditional stepdown.
    const auto opCtx = makeOperationContext();
    ASSERT_EQUALS(ErrorCodes::StaleTerm, getReplCoord()->updateTerm(opCtx.get(), 2));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Ensure that the stepdown command failed.
    auto stepDownStatus = *result.second.get();
    ASSERT_NOT_OK(stepDownStatus);
    // Which code is returned is racy.
    ASSERT(stepDownStatus == ErrorCodes::PrimarySteppedDown ||
           stepDownStatus == ErrorCodes::Interrupted);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));
}

TEST_F(ReplCoordTest, NodeIncludesOtherMembersProgressInUpdatePositionCommand) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
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
    getReplCoord()->setMyLastAppliedOpTime(optime1);
    getReplCoord()->setMyLastDurableOpTime(optime1);
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
            log() << 0;
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime1, appliedOpTime);
            ASSERT_EQUALS(optime1, durableOpTime);
        } else if (memberId == 1) {
            log() << 1;
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime2, appliedOpTime);
            ASSERT_EQUALS(OpTime(), durableOpTime);
        } else if (memberId == 2) {
            log() << 2;
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kAppliedOpTimeFieldName, &appliedOpTime));
            ASSERT_OK(bsonExtractOpTimeField(
                entry, UpdatePositionArgs::kDurableOpTimeFieldName, &durableOpTime));
            ASSERT_EQUALS(optime3, appliedOpTime);
            ASSERT_EQUALS(optime3, durableOpTime);
        } else {
            log() << 3;
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
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));

    // Can't unset maintenance mode if it was never set to begin with.
    Status status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest,
       ReportRollbackWhileInBothRollbackAndMaintenanceModeAndRecoveryAfterFinishingRollback) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    // valid set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // If we go into rollback while in maintenance mode, our state changes to RS_ROLLBACK.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // When we go back to SECONDARY, we still observe RECOVERING because of maintenance mode.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
}

TEST_F(ReplCoordTest, AllowAsManyUnsetMaintenanceModesAsThereHaveBeenSetMaintenanceModes) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    // Can set multiple times
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));

    // Need to unset the number of times you set.
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    Status status = getReplCoord()->setMaintenanceMode(false);
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
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));

    // From rollback, entering and exiting maintenance mode doesn't change perceived
    // state.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // Rollback is sticky even if entered while in maintenance mode.
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, DoNotAllowMaintenanceModeWhilePrimary) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    // Can't modify maintenance mode when PRIMARY
    simulateSuccessfulV1Election();

    Status status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    auto opCtx = makeOperationContext();


    // Step down from primary.
    getReplCoord()->updateTerm(opCtx.get(), getReplCoord()->getTerm() + 1).transitional_ignore();
    ASSERT_OK(getReplCoord()->waitForMemberState(MemberState::RS_SECONDARY, Seconds(1)));

    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
}

TEST_F(ReplCoordTest, DoNotAllowSettingMaintenanceModeWhileConductingAnElection) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));

    // TODO this election shouldn't have to happen.
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();


    // Step down from primary.
    getReplCoord()->updateTerm(opCtx.get(), getReplCoord()->getTerm() + 1).transitional_ignore();
    getReplCoord()
        ->waitForMemberState(MemberState::RS_SECONDARY, Milliseconds(10 * 1000))
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
    Status status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    simulateSuccessfulDryRun();
    ASSERT_EQUALS(TopologyCoordinator::Role::kCandidate, getTopoCoord().getRole());
    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);

    // This cancels the actual election.
    // We do not need to respond to any pending network operations because setFollowerMode() will
    // cancel the vote requester.
    ASSERT_EQUALS(ErrorCodes::ElectionInProgress,
                  getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK));
}

TEST_F(ReplCoordTest,
       NodeReturnsACompleteListOfNodesWeKnowHaveTheWriteDurablyInResponseToGetHostsWrittenTo) {
    HostAndPort myHost("node1:12345");
    HostAndPort client1Host("node2:12345");
    HostAndPort client2Host("node3:12345");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString())
                                          << BSON("_id" << 1 << "host" << client1Host.toString())
                                          << BSON("_id" << 2 << "host" << client2Host.toString()))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString())
                                          << BSON("_id" << 1 << "host" << client1Host.toString())
                                          << BSON("_id" << 2 << "host" << client2Host.toString()))),
                       HostAndPort("node1", 12345));

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);

    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
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

TEST_F(ReplCoordTest, NodeReturnsNoNodesWhenGetOtherNodesInReplSetIsRunBeforeHavingAConfig) {
    start();
    ASSERT_EQUALS(0U, getReplCoord()->getOtherNodesInReplSet().size());
}

TEST_F(ReplCoordTest, NodeReturnsListOfNodesOtherThanItselfInResponseToGetOtherNodesInReplSet) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "h1")
                                          << BSON("_id" << 1 << "host"
                                                        << "h2")
                                          << BSON("_id" << 2 << "host"
                                                        << "h3"
                                                        << "priority"
                                                        << 0
                                                        << "hidden"
                                                        << true))),
                       HostAndPort("h1"));

    std::vector<HostAndPort> otherNodes = getReplCoord()->getOtherNodesInReplSet();
    ASSERT_EQUALS(2U, otherNodes.size());
    if (otherNodes[0] == HostAndPort("h2")) {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[1]);
    } else {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[0]);
        ASSERT_EQUALS(HostAndPort("h2"), otherNodes[1]);
    }
}

TEST_F(ReplCoordTest, IsMasterResponseMentionsLackOfReplicaSetConfig) {
    start();
    IsMasterResponse response;

    getReplCoord()->fillIsMasterForReplSet(&response);
    ASSERT_FALSE(response.isConfigSet());
    BSONObj responseObj = response.toBSON();
    ASSERT_FALSE(responseObj["ismaster"].Bool());
    ASSERT_FALSE(responseObj["secondary"].Bool());
    ASSERT_TRUE(responseObj["isreplicaset"].Bool());
    ASSERT_EQUALS("Does not have a valid replica set config", responseObj["info"].String());

    IsMasterResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response.toBSON()));
}

TEST_F(ReplCoordTest, IsMaster) {
    HostAndPort h1("h1");
    HostAndPort h2("h2");
    HostAndPort h3("h3");
    HostAndPort h4("h4");
    assertStartSuccess(
        BSON(
            "_id"
            << "mySet"
            << "version"
            << 2
            << "members"
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
    getReplCoord()->setMyLastAppliedOpTime(opTime);

    IsMasterResponse response;
    getReplCoord()->fillIsMasterForReplSet(&response);

    ASSERT_EQUALS("mySet", response.getReplSetName());
    ASSERT_EQUALS(2, response.getReplSetVersion());
    ASSERT_FALSE(response.isMaster());
    ASSERT_TRUE(response.isSecondary());
    // TODO(spencer): test that response includes current primary when there is one.
    ASSERT_FALSE(response.isArbiterOnly());
    ASSERT_TRUE(response.isPassive());
    ASSERT_FALSE(response.isHidden());
    ASSERT_TRUE(response.shouldBuildIndexes());
    ASSERT_EQUALS(Seconds(0), response.getSlaveDelay());
    ASSERT_EQUALS(h4, response.getMe());

    std::vector<HostAndPort> hosts = response.getHosts();
    ASSERT_EQUALS(2U, hosts.size());
    if (hosts[0] == h1) {
        ASSERT_EQUALS(h2, hosts[1]);
    } else {
        ASSERT_EQUALS(h2, hosts[0]);
        ASSERT_EQUALS(h1, hosts[1]);
    }
    std::vector<HostAndPort> passives = response.getPassives();
    ASSERT_EQUALS(1U, passives.size());
    ASSERT_EQUALS(h4, passives[0]);
    std::vector<HostAndPort> arbiters = response.getArbiters();
    ASSERT_EQUALS(1U, arbiters.size());
    ASSERT_EQUALS(h3, arbiters[0]);

    stdx::unordered_map<std::string, std::string> tags = response.getTags();
    ASSERT_EQUALS(2U, tags.size());
    ASSERT_EQUALS("value1", tags["key1"]);
    ASSERT_EQUALS("value2", tags["key2"]);
    ASSERT_EQUALS(opTime, response.getLastWriteOpTime());
    ASSERT_EQUALS(lastWriteDate, response.getLastWriteDate());

    IsMasterResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response.toBSON()));
}

TEST_F(ReplCoordTest, IsMasterWithCommittedSnapshot) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    time_t lastWriteDate = 101;
    OpTime opTime = OpTime(Timestamp(lastWriteDate, 2), 1);
    time_t majorityWriteDate = lastWriteDate;
    OpTime majorityOpTime = opTime;

    getReplCoord()->setMyLastAppliedOpTime(opTime);
    getReplCoord()->setMyLastDurableOpTime(opTime);
    ASSERT_EQUALS(majorityOpTime, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    IsMasterResponse response;
    getReplCoord()->fillIsMasterForReplSet(&response);

    ASSERT_EQUALS(opTime, response.getLastWriteOpTime());
    ASSERT_EQUALS(lastWriteDate, response.getLastWriteDate());
    ASSERT_EQUALS(majorityOpTime, response.getLastMajorityWriteOpTime());
    ASSERT_EQUALS(majorityWriteDate, response.getLastMajorityWriteDate());
}

TEST_F(ReplCoordTest, LogAMessageWhenShutDownBeforeReplicationStartUpFinished) {
    init();
    startCapturingLogMessages();
    {
        auto opCtx = makeOperationContext();
        getReplCoord()->shutdown(opCtx.get());
    }
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("shutdown() called before startup() finished"));
}

TEST_F(ReplCoordTest, DoNotProcessSelfWhenUpdatePositionContainsInfoAboutSelf) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);

    // receive updatePosition containing ourself, should not process the update for self
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                                   << 1
                                   << UpdatePositionArgs::kUpdateArrayFieldName
                                   << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                      << 2
                                                      << UpdatePositionArgs::kMemberIdFieldName
                                                      << 0
                                                      << UpdatePositionArgs::kDurableOpTimeFieldName
                                                      << time2.toBSON()
                                                      << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                      << time2.toBSON())))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest, DoNotProcessUpdatePositionWhenItsConfigVersionIsIncorrect) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    // receive updatePosition with incorrect config version
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                                   << 1
                                   << UpdatePositionArgs::kUpdateArrayFieldName
                                   << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                      << 3
                                                      << UpdatePositionArgs::kMemberIdFieldName
                                                      << 1
                                                      << UpdatePositionArgs::kDurableOpTimeFieldName
                                                      << time2.toBSON()
                                                      << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                      << time2.toBSON())))));

    auto opCtx = makeOperationContext();


    long long cfgver;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetUpdatePosition(args, &cfgver));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest, DoNotProcessUpdatePositionOfMembersWhoseIdsAreNotInTheConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTime time1({100, 1}, 1);
    OpTime time2({100, 2}, 1);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    // receive updatePosition with nonexistent member id
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                                   << 1
                                   << UpdatePositionArgs::kUpdateArrayFieldName
                                   << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                      << 2
                                                      << UpdatePositionArgs::kMemberIdFieldName
                                                      << 9
                                                      << UpdatePositionArgs::kDurableOpTimeFieldName
                                                      << time2.toBSON()
                                                      << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                      << time2.toBSON())))));

    auto opCtx = makeOperationContext();


    ASSERT_EQUALS(ErrorCodes::NodeNotFound, getReplCoord()->processReplSetUpdatePosition(args, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

TEST_F(ReplCoordTest,
       ProcessUpdateWhenUpdatePositionContainsOnlyConfigVersionAndMemberIdsWithoutRIDs) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time1(100, 1);
    OpTimeWithTermOne time2(100, 2);
    OpTimeWithTermOne staleTime(10, 0);
    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastDurableOpTime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    // receive a good update position
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastDurableOpTime(time2);
    UpdatePositionArgs args;
    ASSERT_OK(
        args.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                             << 1
                             << UpdatePositionArgs::kUpdateArrayFieldName
                             << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                << 2
                                                << UpdatePositionArgs::kMemberIdFieldName
                                                << 1
                                                << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                << time2.asOpTime().toBSON()
                                                << UpdatePositionArgs::kDurableOpTimeFieldName
                                                << time2.asOpTime().toBSON())
                                           << BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kMemberIdFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                   << time2.asOpTime().toBSON()
                                                   << UpdatePositionArgs::kDurableOpTimeFieldName
                                                   << time2.asOpTime().toBSON())))));

    auto opCtx = makeOperationContext();


    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);

    writeConcern.wNumNodes = 3;
    ASSERT_OK(getReplCoord()->awaitReplication(opCtx.get(), time2, writeConcern).status);
}

void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord, Status* status) {
    auto client = getGlobalServiceContext()->makeClient("rsr");
    auto opCtx = client->makeOperationContext();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345"
                                                      << "priority"
                                                      << 3)
                                           << BSON("_id" << 1 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(opCtx.get(), args, &garbage);
}

TEST_F(ReplCoordTest, AwaitReplicationShouldResolveAsNormalDuringAReconfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));

    // Turn off readconcern majority support, and snapshots.
    disableReadConcernMajoritySupport();
    disableSnapshots();

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 2));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 2));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    ReplicationAwaiter awaiterJournaled(getReplCoord(), getServiceContext());
    writeConcern.wMode = WriteConcernOptions::kMajority;
    awaiterJournaled.setOpTime(time);
    awaiterJournaled.setWriteConcern(writeConcern);
    awaiterJournaled.start();

    // reconfig
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status); });

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

void doReplSetReconfigToFewer(ReplicationCoordinatorImpl* replCoord, Status* status) {
    auto client = getGlobalServiceContext()->makeClient("rsr");
    auto opCtx = client->makeOperationContext();

    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version"
                             << 3
                             << "protocolVersion"
                             << 1
                             << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(opCtx.get(), args, &garbage);
}

TEST_F(
    ReplCoordTest,
    NodeReturnsCannotSatisfyWriteConcernWhenReconfiggingToAClusterThatCannotSatisfyTheWriteConcern) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 2));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 2));
    simulateSuccessfulV1Election();

    OpTimeWithTermOne time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;

    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    ReplicationAwaiter awaiterJournaled(getReplCoord(), getServiceContext());
    writeConcern.wMode = WriteConcernOptions::kMajority;
    awaiterJournaled.setOpTime(time);
    awaiterJournaled.setWriteConcern(writeConcern);
    awaiterJournaled.start();

    // reconfig to fewer nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread([&] { doReplSetReconfigToFewer(getReplCoord(), &status); });

    replyToReceivedHeartbeatV1();

    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and an error should be returned
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern, statusAndDur.status);
    awaiter.reset();
    ReplicationCoordinator::StatusAndDuration statusAndDurJournaled = awaiterJournaled.getResult();
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern, statusAndDurJournaled.status);
    awaiterJournaled.reset();
}

TEST_F(ReplCoordTest,
       NodeReturnsOKFromAwaitReplicationWhenReconfiggingToASetWhereMajorityIsSmallerAndSatisfied) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id"
                                                  << 3)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id"
                                                  << 4))),
                       HostAndPort("node1", 12345));

    // Turn off readconcern majority support, and snapshots.
    disableReadConcernMajoritySupport();
    disableSnapshots();

    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));
    simulateSuccessfulV1Election();

    OpTime time(Timestamp(100, 2), 1);
    auto opCtx = makeOperationContext();

    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);
    ASSERT_OK(getReplCoord()->setLastAppliedOptime_forTest(2, 1, time));


    // majority nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wMode = WriteConcernOptions::kMajority;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;


    ReplicationAwaiter awaiter(getReplCoord(), getServiceContext());
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start();

    // demonstrate that majority cannot currently be satisfied
    WriteConcernOptions writeConcern2;
    writeConcern2.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern2.wMode = WriteConcernOptions::kMajority;
    writeConcern.syncMode = WriteConcernOptions::SyncMode::NONE;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(opCtx.get(), time, writeConcern2).status);

    // reconfig to three nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread([&] { doReplSetReconfig(getReplCoord(), &status); });

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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id"
                                                  << 3
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id"
                                                  << 4
                                                  << "arbiterOnly"
                                                  << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime time(Timestamp(100, 1), 1);
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);
    simulateSuccessfulV1Election();

    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id"
                                                  << 3
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id"
                                                  << 4
                                                  << "arbiterOnly"
                                                  << true))),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime zero(Timestamp(0, 0), 0);
    OpTime time(Timestamp(100, 1), 1);
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);
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
    getReplCoord()->setMyLastAppliedOpTime(newTime);
    getReplCoord()->setMyLastDurableOpTime(newTime);
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
class StableOpTimeTest : public ReplCoordTest {
protected:
    /**
     * Return a string representation of the given set 's'.
     */
    std::string opTimeSetString(std::set<OpTime> s) {
        std::stringstream ss;
        ss << "{ ";
        for (auto const& el : s) {
            ss << el << " ";
        }
        ss << "}";
        return ss.str();
    }

    void initReplSetMode() {
        auto settings = ReplSettings();
        settings.setReplSetString("replset");
        init(settings);
    }
};

// An equality assertion for two std::set<OpTime> values. Prints the elements of each set when
// the assertion fails. Only to be used in a 'StableOpTimeTest' unit test function.
#define ASSERT_OPTIME_SET_EQ(a, b) \
    ASSERT(a == b) << (opTimeSetString(a) + " != " + opTimeSetString(b));

TEST_F(StableOpTimeTest, CalculateStableOpTime) {

    /**
     * Tests the 'ReplicationCoordinatorImpl::_calculateStableOpTime' method.
     */

    initReplSetMode();
    auto repl = getReplCoord();
    OpTime commitPoint;
    boost::optional<OpTime> expectedStableOpTime, stableOpTime;
    std::set<OpTime> stableOpTimeCandidates;
    long long term = 0;

    // There is a valid stable optime less than the commit point.
    commitPoint = OpTime({0, 3}, term);
    stableOpTimeCandidates = {
        OpTime({0, 0}, term), OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 4}, term)};
    expectedStableOpTime = OpTime({0, 2}, term);
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is a valid stable optime equal to the commit point.
    commitPoint = OpTime({0, 2}, term);
    stableOpTimeCandidates = {
        OpTime({0, 0}, term), OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term)};
    expectedStableOpTime = OpTime({0, 2}, term);
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is a valid stable optime, all candidates are smaller than the commit point.
    commitPoint = OpTime({0, 4}, term);
    stableOpTimeCandidates = {OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term)};
    expectedStableOpTime = OpTime({0, 3}, term);
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is no valid stable optime, all candidates are greater than the commit point.
    commitPoint = OpTime({0, 0}, term);
    stableOpTimeCandidates = {OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term)};
    expectedStableOpTime = boost::none;
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There are no timestamp candidates.
    commitPoint = OpTime({0, 0}, term);
    stableOpTimeCandidates = {};
    expectedStableOpTime = boost::none;
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is a single timestamp candidate which is equal to the commit point.
    commitPoint = OpTime({0, 1}, term);
    stableOpTimeCandidates = {OpTime({0, 1}, term)};
    expectedStableOpTime = OpTime({0, 1}, term);
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is a single timestamp candidate which is greater than the commit point.
    commitPoint = OpTime({0, 0}, term);
    stableOpTimeCandidates = {OpTime({0, 1}, term)};
    expectedStableOpTime = boost::none;
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);

    // There is a single timestamp candidate which is less than the commit point.
    commitPoint = OpTime({0, 2}, term);
    stableOpTimeCandidates = {OpTime({0, 1}, term)};
    expectedStableOpTime = OpTime({0, 1}, term);
    stableOpTime = repl->calculateStableOpTime_forTest(stableOpTimeCandidates, commitPoint);
    ASSERT_EQ(expectedStableOpTime, stableOpTime);
}

TEST_F(StableOpTimeTest, CleanupStableOpTimeCandidates) {

    /**
     * Tests the 'ReplicationCoordinatorImpl::_cleanupStableOpTimeCandidates' method.
     */

    initReplSetMode();
    auto repl = getReplCoord();
    OpTime stableOpTime;
    std::set<OpTime> opTimeCandidates, expectedOpTimeCandidates;
    long long term = 0;

    // Cleanup should remove all timestamp candidates < the stable optime.
    stableOpTime = OpTime({0, 3}, term);
    opTimeCandidates = {
        OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term), OpTime({0, 4}, term)};
    expectedOpTimeCandidates = {OpTime({0, 3}, term), OpTime({0, 4}, term)};
    repl->cleanupStableOpTimeCandidates_forTest(&opTimeCandidates, stableOpTime);
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Cleanup should remove all timestamp candidates if they are all < the stable optime.
    stableOpTime = OpTime({0, 5}, term);
    opTimeCandidates = {
        OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term), OpTime({0, 4}, term)};
    expectedOpTimeCandidates = {};
    repl->cleanupStableOpTimeCandidates_forTest(&opTimeCandidates, stableOpTime);
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Cleanup should have no effect when stable optime is less than all candidates.
    stableOpTime = OpTime({0, 0}, term);
    opTimeCandidates = {
        OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term), OpTime({0, 4}, term)};
    expectedOpTimeCandidates = {
        OpTime({0, 1}, term), OpTime({0, 2}, term), OpTime({0, 3}, term), OpTime({0, 4}, term)};
    repl->cleanupStableOpTimeCandidates_forTest(&opTimeCandidates, stableOpTime);
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Cleanup should have no effect for a single candidate that is equal to stable optime.
    stableOpTime = OpTime({0, 1}, term);
    opTimeCandidates = {OpTime({0, 1}, term)};
    expectedOpTimeCandidates = {OpTime({0, 1}, term)};
    repl->cleanupStableOpTimeCandidates_forTest(&opTimeCandidates, stableOpTime);
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Cleanup should leave an empty candidate list unchanged.
    stableOpTime = OpTime({0, 0}, term);
    opTimeCandidates = {};
    expectedOpTimeCandidates = {};
    repl->cleanupStableOpTimeCandidates_forTest(&opTimeCandidates, stableOpTime);
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);
}


TEST_F(StableOpTimeTest, SetMyLastAppliedSetsStableOpTimeForStorage) {

    /**
     * Test that 'setMyLastAppliedOpTime' sets the stable timestamp properly for the storage engine
     * and that timestamp cleanup occurs. This test is not meant to fully exercise the stable
     * optime calculation logic.
     */
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));

    auto repl = getReplCoord();
    Timestamp stableTimestamp;
    long long term = 2;

    getStorageInterface()->supportsDocLockingBool = true;

    repl->advanceCommitPoint(OpTime({1, 1}, term));
    ASSERT_EQUALS(Timestamp::min(), getStorageInterface()->getStableTimestamp());
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(1, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(1, 1));
    simulateSuccessfulV1Election();

    repl->advanceCommitPoint(OpTime({2, 2}, term));
    ASSERT_EQUALS(Timestamp(1, 1), getStorageInterface()->getStableTimestamp());

    // Check that the stable timestamp is not updated if the all-committed timestamp is behind.
    getStorageInterface()->allCommittedTimestamp = Timestamp(1, 1);
    repl->setMyLastAppliedOpTime(OpTime({1, 2}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(1, 1), getStorageInterface()->getStableTimestamp());

    getStorageInterface()->allCommittedTimestamp = Timestamp(3, 1);

    // Check that the stable timestamp is updated for the storage engine when we set the applied
    // optime.
    repl->setMyLastAppliedOpTime(OpTime({2, 1}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    // Check that timestamp cleanup occurs.
    repl->setMyLastAppliedOpTime(OpTime({2, 2}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 2), stableTimestamp);

    auto opTimeCandidates = repl->getStableOpTimeCandidates_forTest();
    std::set<OpTime> expectedOpTimeCandidates = {OpTime({2, 2}, term)};
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);
}

TEST_F(StableOpTimeTest, AdvanceCommitPointSetsStableOpTimeForStorage) {

    /**
     * Test that 'advanceCommitPoint' sets the stable optime for the storage engine and that
     * timestamp cleanup occurs. This test is not meant to fully exercise the stable optime
     * calculation logic.
     */

    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test3:1234"))),
                       HostAndPort("test2", 1234));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(1, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(1, 1));
    simulateSuccessfulV1Election();

    auto repl = getReplCoord();
    Timestamp stableTimestamp;
    long long term = 2;

    getStorageInterface()->supportsDocLockingBool = true;
    getStorageInterface()->allCommittedTimestamp = Timestamp(2, 1);

    // Add three stable optime candidates.
    repl->setMyLastAppliedOpTime(OpTime({2, 1}, term));
    repl->setMyLastAppliedOpTime(OpTime({2, 2}, term));
    repl->setMyLastAppliedOpTime(OpTime({3, 2}, term));

    // Set a commit point and check the stable optime.
    repl->advanceCommitPoint(OpTime({2, 1}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    // Check that the stable timestamp is not updated if the all-committed timestamp is behind.
    repl->advanceCommitPoint(OpTime({2, 2}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(2, 1), stableTimestamp);

    getStorageInterface()->allCommittedTimestamp = Timestamp(4, 4);

    // Check that the stable timestamp is updated when we advance the commit point.
    repl->advanceCommitPoint(OpTime({3, 2}, term));
    stableTimestamp = getStorageInterface()->getStableTimestamp();
    ASSERT_EQUALS(Timestamp(3, 2), stableTimestamp);

    // Check that timestamp candidate cleanup occurs.
    auto opTimeCandidates = getReplCoord()->getStableOpTimeCandidates_forTest();
    std::set<OpTime> expectedOpTimeCandidates = {OpTime({3, 2}, term)};
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);
}

TEST_F(StableOpTimeTest, ClearOpTimeCandidatesPastCommonPointAfterRollback) {

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));

    auto repl = getReplCoord();
    long long term = 0;
    ASSERT_OK(repl->setFollowerMode(MemberState::RS_SECONDARY));

    OpTime rollbackCommonPoint = OpTime({1, 2}, term);
    OpTime commitPoint = OpTime({1, 2}, term);
    repl->advanceCommitPoint(commitPoint);
    ASSERT_EQUALS(Timestamp::min(), getStorageInterface()->getStableTimestamp());

    repl->setMyLastAppliedOpTime(OpTime({1, 1}, term));
    repl->setMyLastAppliedOpTime(OpTime({1, 2}, term));
    repl->setMyLastAppliedOpTime(OpTime({1, 3}, term));
    repl->setMyLastAppliedOpTime(OpTime({1, 4}, term));

    // The stable timestamp should be equal to the commit point timestamp.
    const Timestamp stableTimestamp = getStorageInterface()->getStableTimestamp();
    Timestamp expectedStableTimestamp = commitPoint.getTimestamp();
    ASSERT_EQUALS(expectedStableTimestamp, stableTimestamp);

    // The stable optime candidate set should contain optimes >= the stable optime.
    std::set<OpTime> opTimeCandidates = repl->getStableOpTimeCandidates_forTest();
    std::set<OpTime> expectedOpTimeCandidates = {
        OpTime({1, 2}, term), OpTime({1, 3}, term), OpTime({1, 4}, term)};
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Transition to ROLLBACK. The set of stable optime candidates should not have changed.
    ASSERT_OK(repl->setFollowerMode(MemberState::RS_ROLLBACK));
    opTimeCandidates = repl->getStableOpTimeCandidates_forTest();
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);

    // Simulate a rollback to the common point.
    auto opCtx = makeOperationContext();
    getExternalState()->setLastOpTime(rollbackCommonPoint);
    repl->resetLastOpTimesFromOplog(opCtx.get(),
                                    ReplicationCoordinator::DataConsistency::Inconsistent);

    // Transition to RECOVERING from ROLLBACK.
    ASSERT_OK(repl->setFollowerMode(MemberState::RS_RECOVERING));

    // Make sure the stable optime candidate set has been cleared of all entries past the common
    // point.
    opTimeCandidates = repl->getStableOpTimeCandidates_forTest();
    auto stableOpTime = repl->getStableOpTime_forTest();
    ASSERT(stableOpTime);
    expectedOpTimeCandidates = {*stableOpTime};
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, opTimeCandidates);
}

TEST_F(StableOpTimeTest, OpTimeCandidatesAreNotAddedWhenStateIsNotConsistent) {

    initReplSetMode();
    auto repl = getReplCoord();
    long long term = 0;

    OpTime consistentOpTime = OpTime({1, 1}, term);
    OpTime inconsistentOpTime = OpTime({1, 2}, term);
    std::set<OpTime> expectedOpTimeCandidates = {OpTime({1, 1}, term)};

    // Set the lastApplied optime forward when data is consistent, and check that it was added to
    // the candidate set.
    repl->setMyLastAppliedOpTimeForward(consistentOpTime,
                                        ReplicationCoordinator::DataConsistency::Consistent);
    ASSERT_EQUALS(consistentOpTime, repl->getMyLastAppliedOpTime());
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, repl->getStableOpTimeCandidates_forTest());

    // Set the lastApplied optime forward when data is not consistent, and check that it wasn't
    // added to the candidate set.
    repl->setMyLastAppliedOpTimeForward(inconsistentOpTime,
                                        ReplicationCoordinator::DataConsistency::Inconsistent);
    ASSERT_EQUALS(inconsistentOpTime, repl->getMyLastAppliedOpTime());
    ASSERT_OPTIME_SET_EQ(expectedOpTimeCandidates, repl->getStableOpTimeCandidates_forTest());
}


TEST_F(ReplCoordTest, NodeReturnsShutdownInProgressWhenWaitingUntilAnOpTimeDuringShutdown) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(10, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(10, 1));

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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(10, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(10, 1));

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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(opCtx.get(), ReadConcernArgs()));
}

TEST_F(ReplCoordTest, NodeReturnsOkImmediatelyWhenWaitingUntilOpTimePassesAnOpTimePriorToOurLast) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastAppliedOpTime(OpTimeWithTermOne(100, 1));
    getReplCoord()->setMyLastDurableOpTime(OpTimeWithTermOne(100, 1));

    auto opCtx = makeOperationContext();

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTimeWithTermOne(50, 0), ReadConcernLevel::kLocalReadConcern)));
}

TEST_F(ReplCoordTest, NodeReturnsOkImmediatelyWhenWaitingUntilOpTimePassesAnOpTimeEqualToOurLast) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));


    OpTimeWithTermOne time(100, 1);
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);

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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 1), 0));

    shutdown(opCtx.get());

    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));
    ASSERT_EQUALS(status, ErrorCodes::ShutdownInProgress);
}

TEST_F(ReplCoordTest, ReadAfterCommittedInterrupted) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));
    const auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(10, 1), 0));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(10, 1), 0));
    killOperation(opCtx.get());
    auto status = getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 0), ReadConcernLevel::kMajorityReadConcern));
    ASSERT_EQUALS(status, ErrorCodes::Interrupted);
}

TEST_F(ReplCoordTest, ReadAfterCommittedGreaterOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(100, 1), 1));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(100, 1), 1));

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(50, 0), 1), ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedEqualOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    OpTime time(Timestamp(100, 1), 1);
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(), ReadConcernArgs(time, ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredGreaterOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(0, 0), 1));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(0, 0), 1));
    OpTime committedOpTime(Timestamp(200, 1), 1);
    auto pseudoLogOp = stdx::async(stdx::launch::async, [this, &committedOpTime]() {
        // Not guaranteed to be scheduled after waitUntil blocks...
        getReplCoord()->setMyLastAppliedOpTime(committedOpTime);
        getReplCoord()->setMyLastDurableOpTime(committedOpTime);
    });

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(),
        ReadConcernArgs(OpTime(Timestamp(100, 0), 1), ReadConcernLevel::kMajorityReadConcern)));
}

TEST_F(ReplCoordTest, ReadAfterCommittedDeferredEqualOpTime) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));
    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());
    getReplCoord()->setMyLastAppliedOpTime(OpTime(Timestamp(0, 0), 1));
    getReplCoord()->setMyLastDurableOpTime(OpTime(Timestamp(0, 0), 1));

    OpTime opTimeToWait(Timestamp(100, 1), 1);

    auto pseudoLogOp = stdx::async(stdx::launch::async, [this, &opTimeToWait]() {
        // Not guaranteed to be scheduled after waitUntil blocks...
        getReplCoord()->setMyLastAppliedOpTime(opTimeToWait);
        getReplCoord()->setMyLastDurableOpTime(opTimeToWait);
    });

    ASSERT_OK(getReplCoord()->waitUntilOpTimeForRead(
        opCtx.get(), ReadConcernArgs(opTimeToWait, ReadConcernLevel::kMajorityReadConcern)));
    pseudoLogOp.get();
}

TEST_F(ReplCoordTest, IgnoreTheContentsOfMetadataWhenItsConfigVersionDoesNotMatchOurs) {
    // Ensure that we do not process ReplSetMetadata when ConfigVersions do not match.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // lower configVersion
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(10, 0) << "t" << 2)
                                  << "configVersion"
                                  << 1
                                  << "primaryIndex"
                                  << 2
                                  << "term"
                                  << 2
                                  << "syncSourceIndex"
                                  << 1)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(0, getReplCoord()->getTerm());

    // higher configVersion
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 2) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(10, 0) << "t" << 2)
                                  << "configVersion"
                                  << 100
                                  << "primaryIndex"
                                  << 2
                                  << "term"
                                  << 2
                                  << "syncSourceIndex"
                                  << 1)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(0, getReplCoord()->getTerm());
}

TEST_F(ReplCoordTest, UpdateLastCommittedOpTimeWhenTheLastCommittedOpTimeIsNewer) {
    // Ensure that LastCommittedOpTime updates when a newer OpTime comes in via ReplSetMetadata,
    // but not if the OpTime is older than the current LastCommittedOpTime.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    OpTime time(Timestamp(10, 1), 1);
    OpTime oldTime(Timestamp(9, 1), 1);
    getReplCoord()->setMyLastAppliedOpTime(time);

    // higher OpTime, should change
    getReplCoord()->advanceCommitPoint(time);
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(time, getReplCoord()->getCurrentCommittedSnapshotOpTime());

    // lower OpTime, should not change
    getReplCoord()->advanceCommitPoint(oldTime);
    ASSERT_EQUALS(time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(time, getReplCoord()->getCurrentCommittedSnapshotOpTime());
}
TEST_F(ReplCoordTest, UpdateTermWhenTheTermFromMetadataIsNewerButNeverUpdateCurrentPrimaryIndex) {
    // Ensure that the term is updated if and only if the term is greater than our current term.
    // Ensure that currentPrimaryIndex is never altered by ReplSetMetadata.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    // higher term, should change
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 3) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(10, 0) << "t" << 3)
                                  << "configVersion"
                                  << 2
                                  << "primaryIndex"
                                  << 2
                                  << "term"
                                  << 3
                                  << "syncSourceIndex"
                                  << 1)));
    getReplCoord()->processReplSetMetadata(metadata.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // lower term, should not change
    StatusWith<rpc::ReplSetMetadata> metadata2 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(11, 0) << "t" << 3)
                                  << "configVersion"
                                  << 2
                                  << "primaryIndex"
                                  << 1
                                  << "term"
                                  << 2
                                  << "syncSourceIndex"
                                  << 1)));
    getReplCoord()->processReplSetMetadata(metadata2.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());

    // same term, should not change
    StatusWith<rpc::ReplSetMetadata> metadata3 = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(11, 0) << "t" << 3) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(11, 0) << "t" << 3)
                                  << "configVersion"
                                  << 2
                                  << "primaryIndex"
                                  << 1
                                  << "term"
                                  << 3
                                  << "syncSourceIndex"
                                  << 1)));
    getReplCoord()->processReplSetMetadata(metadata3.getValue());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest,
       LastCommittedOpTimeNotUpdatedEvenWhenHeartbeatResponseWithMetadataHasFresherValues) {
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    auto opCtx = makeOperationContext();
    getReplCoord()->updateTerm(opCtx.get(), 1).transitional_ignore();
    ASSERT_EQUALS(1, getReplCoord()->getTerm());

    auto replCoord = getReplCoord();
    auto config = replCoord->getConfig();

    // Higher term - should update term but not last committed optime.
    StatusWith<rpc::ReplSetMetadata> metadata = rpc::ReplSetMetadata::readFromMetadata(BSON(
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 0) << "t" << 3) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(10, 0) << "t" << 3)
                                  << "configVersion"
                                  << config.getConfigVersion()
                                  << "primaryIndex"
                                  << 1
                                  << "term"
                                  << 3
                                  << "syncSourceIndex"
                                  << 1)));
    BSONObjBuilder metadataBuilder;
    ASSERT_OK(metadata.getValue().writeToMetadata(&metadataBuilder));
    auto metadataObj = metadataBuilder.obj();

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
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true), metadataObj));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(OpTime(Timestamp(0, 0), 0), getReplCoord()->getLastCommittedOpTime());
    ASSERT_EQUALS(3, getReplCoord()->getTerm());
    ASSERT_EQUALS(-1, getTopoCoord().getCurrentPrimaryIndex());
}

TEST_F(ReplCoordTest, PrepareOplogQueryMetadata) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))
                            << "protocolVersion"
                            << 1),
                       HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

    OpTime optime1{Timestamp(10, 0), 3};
    OpTime optime2{Timestamp(11, 2), 5};

    getReplCoord()->advanceCommitPoint(optime1);
    getReplCoord()->setMyLastAppliedOpTime(optime2);

    auto opCtx = makeOperationContext();

    BSONObjBuilder metadataBob;
    getReplCoord()->prepareReplMetadata(
        BSON(rpc::kOplogQueryMetadataFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
        OpTime(),
        &metadataBob);

    BSONObj metadata = metadataBob.done();
    log() << metadata;

    auto oqMetadata = rpc::OplogQueryMetadata::readFromMetadata(metadata);
    ASSERT_OK(oqMetadata.getStatus());
    ASSERT_EQ(oqMetadata.getValue().getLastOpCommitted(), optime1);
    ASSERT_EQ(oqMetadata.getValue().getLastOpApplied(), optime2);
    ASSERT_EQ(oqMetadata.getValue().getRBID(), 100);
    ASSERT_EQ(oqMetadata.getValue().getSyncSourceIndex(), -1);
    ASSERT_EQ(oqMetadata.getValue().getPrimaryIndex(), -1);

    auto replMetadata = rpc::ReplSetMetadata::readFromMetadata(metadata);
    ASSERT_OK(replMetadata.getStatus());
    ASSERT_EQ(replMetadata.getValue().getLastOpCommitted(), optime1);
    ASSERT_EQ(replMetadata.getValue().getLastOpVisible(), OpTime());
    ASSERT_EQ(replMetadata.getValue().getConfigVersion(), 2);
    ASSERT_EQ(replMetadata.getValue().getTerm(), 0);
    ASSERT_EQ(replMetadata.getValue().getSyncSourceIndex(), -1);
    ASSERT_EQ(replMetadata.getValue().getPrimaryIndex(), -1);
}

TEST_F(ReplCoordTest, TermAndLastCommittedOpTimeUpdatedFromHeartbeatWhenArbiter) {
    // Ensure that the metadata is processed if it is contained in a heartbeat response.
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0
                                               << "arbiterOnly"
                                               << true)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))
                            << "protocolVersion"
                            << 1),
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
        rpc::kReplSetMetadataFieldName
        << BSON("lastOpCommitted" << BSON("ts" << Timestamp(10, 1) << "t" << 3) << "lastOpVisible"
                                  << BSON("ts" << Timestamp(10, 1) << "t" << 3)
                                  << "configVersion"
                                  << config.getConfigVersion()
                                  << "primaryIndex"
                                  << 1
                                  << "term"
                                  << 3
                                  << "syncSourceIndex"
                                  << 1)));
    BSONObjBuilder metadataBuilder;
    ASSERT_OK(metadata.getValue().writeToMetadata(&metadataBuilder));
    auto metadataObj = metadataBuilder.obj();

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
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true), metadataObj));
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
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
    log() << "black holing " << noi->getRequest().cmdObj;
    net->blackHole(noi);

    // Advance simulator clock to some time before the first scheduled election.
    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    log() << "Election initially scheduled at " << electionTimeoutWhen << " (simulator time)";
    ASSERT_GREATER_THAN(electionTimeoutWhen, net->now());
    auto until = net->now() + (electionTimeoutWhen - net->now()) / 2;
    net->runUntil(until);
    ASSERT_EQUALS(until, net->now());
    net->exitNetwork();

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_LESS_THAN_OR_EQUALS(until + replCoord->getConfig().getElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, DoNotScheduleElectionWhenCancelAndRescheduleElectionTimeoutIsRunInRollback) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
                       HostAndPort("node1", 12345));
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_ROLLBACK));

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_EQUALS(Date_t(), electionTimeoutWhen);
}

TEST_F(ReplCoordTest,
       DoNotScheduleElectionWhenCancelAndRescheduleElectionTimeoutIsRunWhileUnelectable) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0
                                               << "priority"
                                               << 0
                                               << "hidden"
                                               << true)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);
    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command with a config that excludes node1.
    ReplSetHeartbeatResponse hbResp;
    ReplSetConfig config;
    config
        .initialize(BSON("_id"
                         << "mySet"
                         << "protocolVersion"
                         << 1
                         << "version"
                         << 3
                         << "members"
                         << BSON_ARRAY(BSON("host"
                                            << "node2:12345"
                                            << "_id"
                                            << 1))))
        .transitional_ignore();
    hbResp.setConfig(config);
    hbResp.setConfigVersion(3);
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true)));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_OK(getReplCoord()->waitForMemberState(MemberState::RS_REMOVED, Seconds(1)));
    ASSERT_EQUALS(config.getConfigVersion(), getReplCoord()->getConfig().getConfigVersion());

    getReplCoord()->cancelAndRescheduleElectionTimeout();

    ASSERT_EQUALS(Date_t(), replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       RescheduleElectionTimeoutWhenProcessingHeartbeatResponseFromPrimaryInSameTerm) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_PRIMARY);
    hbResp.setTerm(replCoord->getTerm());

    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON(true)));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_LESS_THAN_OR_EQUALS(heartbeatWhen + replCoord->getConfig().getElectionTimeoutPeriod(),
                               replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       DontRescheduleElectionTimeoutWhenProcessingHeartbeatResponseFromPrimaryInDiffertTerm) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
    log() << "processing " << request.cmdObj;
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
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON(true)));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_GREATER_THAN(heartbeatWhen + replCoord->getConfig().getElectionTimeoutPeriod(),
                        replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest,
       CancelAndRescheduleElectionTimeoutWhenProcessingHeartbeatResponseWithoutState) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "protocolVersion"
                            << 1
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1))),
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
    log() << "processing " << request.cmdObj;
    ASSERT_EQUALS(HostAndPort("node2", 12345), request.target);

    ASSERT_EQUALS("replSetHeartbeat", request.cmdObj.firstElement().fieldNameStringData());

    // Respond to node1's heartbeat command to indicate that node2 is PRIMARY.
    ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    // Heartbeat response is scheduled with a delay so that we can be sure that
    // the election was rescheduled due to the heartbeat response.
    auto heartbeatWhen = net->now() + Seconds(1);
    net->scheduleResponse(noi, heartbeatWhen, makeResponseStatus(hbResp.toBSON(true)));
    net->runUntil(heartbeatWhen);
    ASSERT_EQUALS(heartbeatWhen, net->now());
    net->runReadyNetworkOperations();
    net->exitNetwork();

    // Election timeout should remain unchanged.
    ASSERT_EQUALS(electionTimeoutWhen, replCoord->getElectionTimeout_forTest());
}

TEST_F(ReplCoordTest, AdvanceCommittedSnapshotToMostRecentSnapshotPriorToOpTimeWhenOpTimeChanges) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
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

    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastAppliedOpTime(time5);

    // ensure current snapshot follows price is right rules (closest but not greater than)

    getReplCoord()->setMyLastDurableOpTime(time3);
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getReplCoord()->setMyLastDurableOpTime(time4);
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getReplCoord()->setMyLastDurableOpTime(time5);
    ASSERT_EQUALS(time5, getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest, ZeroCommittedSnapshotWhenAllSnapshotsAreDropped) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
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

    getReplCoord()->setMyLastAppliedOpTime(time1);
    getReplCoord()->setMyLastAppliedOpTime(time2);
    getReplCoord()->setMyLastAppliedOpTime(time5);
    getReplCoord()->setMyLastDurableOpTime(time5);

    // ensure dropping all snapshots should reset the current committed snapshot
    getReplCoord()->dropAllSnapshots();
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest, DoNotAdvanceCommittedSnapshotWhenAppliedOpTimeChanges) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));

    auto opCtx = makeOperationContext();
    runSingleNodeElection(opCtx.get());

    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);

    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getReplCoord()->setMyLastAppliedOpTime(time2);
    ASSERT_EQUALS(OpTime(), getReplCoord()->getCurrentCommittedSnapshotOpTime());
    getReplCoord()->setMyLastDurableOpTime(time2);
    ASSERT_EQUALS(time2, getReplCoord()->getCurrentCommittedSnapshotOpTime());
}

TEST_F(ReplCoordTest,
       NodeChangesMyLastOpTimeWhenAndOnlyWhensetMyLastDurableOpTimeReceivesANewerOpTime4DurableSE) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0))),
                       HostAndPort("node1", 12345));


    OpTime time1(Timestamp(100, 1), 1);
    OpTime time2(Timestamp(100, 2), 1);
    OpTime time3(Timestamp(100, 3), 1);

    auto consistency = ReplicationCoordinator::DataConsistency::Consistent;
    getReplCoord()->setMyLastAppliedOpTime(time1);
    ASSERT_EQUALS(time1, getReplCoord()->getMyLastAppliedOpTime());
    getReplCoord()->setMyLastAppliedOpTimeForward(time3, consistency);
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastAppliedOpTime());
    getReplCoord()->setMyLastAppliedOpTimeForward(time2, consistency);
    getReplCoord()->setMyLastDurableOpTimeForward(time2);
    ASSERT_EQUALS(time3, getReplCoord()->getMyLastAppliedOpTime());
}

TEST_F(ReplCoordTest, OnlyForwardSyncProgressForOtherNodesWhenTheNodesAreBelievedToBeUp) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version"
             << 1
             << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234")
                           << BSON("_id" << 2 << "host"
                                         << "test3:1234"))
             << "protocolVersion"
             << 1
             << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("test1", 1234));
    OpTime optime(Timestamp(100, 2), 0);
    getReplCoord()->setMyLastAppliedOpTime(optime);
    getReplCoord()->setMyLastDurableOpTime(optime);
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
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version"
             << 1
             << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234")
                           << BSON("_id" << 2 << "host"
                                         << "test3:1234"))
             << "protocolVersion"
             << 1
             << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("test1", 1234));
    OpTime optime(Timestamp(100, 2), 0);
    getReplCoord()->setMyLastAppliedOpTime(optime);
    getReplCoord()->setMyLastDurableOpTime(optime);
    auto opCtx = makeOperationContext();

    // Set last committed optime via metadata.
    rpc::ReplSetMetadata syncSourceMetadata(optime.getTerm(), optime, optime, 1, OID(), -1, 1);
    getReplCoord()->processReplSetMetadata(syncSourceMetadata);
    getReplCoord()->advanceCommitPoint(optime);

    BSONObj cmd = unittest::assertGet(getReplCoord()->prepareReplSetUpdatePositionCommand());
    auto metadata = unittest::assertGet(rpc::ReplSetMetadata::readFromMetadata(cmd));
    ASSERT_EQUALS(metadata.getTerm(), getReplCoord()->getTerm());
    ASSERT_EQUALS(metadata.getLastOpVisible(), optime);

    auto oqMetadataStatus = rpc::OplogQueryMetadata::readFromMetadata(cmd);
    ASSERT_EQUALS(oqMetadataStatus.getStatus(), ErrorCodes::NoSuchKey);
}

TEST_F(ReplCoordTest, StepDownWhenHandleLivenessTimeoutMarksAMajorityOfVotingNodesDown) {
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version"
             << 2
             << "members"
             << BSON_ARRAY(BSON("host"
                                << "node1:12345"
                                << "_id"
                                << 0)
                           << BSON("host"
                                   << "node2:12345"
                                   << "_id"
                                   << 1)
                           << BSON("host"
                                   << "node3:12345"
                                   << "_id"
                                   << 2)
                           << BSON("host"
                                   << "node4:12345"
                                   << "_id"
                                   << 3)
                           << BSON("host"
                                   << "node5:12345"
                                   << "_id"
                                   << 4))
             << "protocolVersion"
             << 1
             << "settings"
             << BSON("electionTimeoutMillis" << 2000 << "heartbeatIntervalMillis" << 40000)),
        HostAndPort("node1", 12345));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTime startingOpTime = OpTime(Timestamp(100, 1), 0);
    getReplCoord()->setMyLastAppliedOpTime(startingOpTime);
    getReplCoord()->setMyLastDurableOpTime(startingOpTime);

    // Receive notification that every node is up.
    UpdatePositionArgs args;
    ASSERT_OK(
        args.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                             << 1
                             << UpdatePositionArgs::kUpdateArrayFieldName
                             << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                << 2
                                                << UpdatePositionArgs::kMemberIdFieldName
                                                << 1
                                                << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                << startingOpTime.toBSON()
                                                << UpdatePositionArgs::kDurableOpTimeFieldName
                                                << startingOpTime.toBSON())
                                           << BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kMemberIdFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                   << startingOpTime.toBSON()
                                                   << UpdatePositionArgs::kDurableOpTimeFieldName
                                                   << startingOpTime.toBSON())
                                           << BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kMemberIdFieldName
                                                   << 3
                                                   << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                   << startingOpTime.toBSON()
                                                   << UpdatePositionArgs::kDurableOpTimeFieldName
                                                   << startingOpTime.toBSON())
                                           << BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                   << 2
                                                   << UpdatePositionArgs::kMemberIdFieldName
                                                   << 4
                                                   << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                   << startingOpTime.toBSON()
                                                   << UpdatePositionArgs::kDurableOpTimeFieldName
                                                   << startingOpTime.toBSON())))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    // Become PRIMARY.
    simulateSuccessfulV1Election();

    // Keep two nodes alive via UpdatePosition.
    UpdatePositionArgs args1;
    ASSERT_OK(
        args1.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                              << 1
                              << UpdatePositionArgs::kUpdateArrayFieldName
                              << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                 << 2
                                                 << UpdatePositionArgs::kMemberIdFieldName
                                                 << 1
                                                 << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                 << startingOpTime.toBSON()
                                                 << UpdatePositionArgs::kDurableOpTimeFieldName
                                                 << startingOpTime.toBSON())
                                            << BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                    << 2
                                                    << UpdatePositionArgs::kMemberIdFieldName
                                                    << 2
                                                    << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                    << startingOpTime.toBSON()
                                                    << UpdatePositionArgs::kDurableOpTimeFieldName
                                                    << startingOpTime.toBSON())))));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    getNet()->runUntil(startDate + Milliseconds(100));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args1, 0));

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
    ASSERT_OK(
        args2.initialize(BSON(UpdatePositionArgs::kCommandFieldName
                              << 1
                              << UpdatePositionArgs::kUpdateArrayFieldName
                              << BSON_ARRAY(BSON(UpdatePositionArgs::kConfigVersionFieldName
                                                 << 2
                                                 << UpdatePositionArgs::kMemberIdFieldName
                                                 << 1
                                                 << UpdatePositionArgs::kDurableOpTimeFieldName
                                                 << startingOpTime.toBSON()
                                                 << UpdatePositionArgs::kAppliedOpTimeFieldName
                                                 << startingOpTime.toBSON())))));
    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args2, 0));

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

    ASSERT_OK(getReplCoord()->waitForMemberState(MemberState::RS_SECONDARY, Hours{1}));
}

TEST_F(ReplCoordTest, WaitForMemberState) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto replCoord = getReplCoord();
    auto initialTerm = replCoord->getTerm();
    replCoord->setMyLastAppliedOpTime(OpTime(Timestamp(1, 1), 0));
    replCoord->setMyLastDurableOpTime(OpTime(Timestamp(1, 1), 0));
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    // Single node cluster - this node should start election on setFollowerMode() completion.
    replCoord->waitForElectionFinish_forTest();

    // Successful dry run election increases term.
    ASSERT_EQUALS(initialTerm + 1, replCoord->getTerm());

    auto timeout = Milliseconds(1);
    ASSERT_OK(replCoord->waitForMemberState(MemberState::RS_PRIMARY, timeout));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit,
                  replCoord->waitForMemberState(MemberState::RS_REMOVED, timeout));

    ASSERT_EQUALS(ErrorCodes::BadValue,
                  replCoord->waitForMemberState(MemberState::RS_PRIMARY, Milliseconds(-1)));

    // Zero timeout is fine.
    ASSERT_OK(replCoord->waitForMemberState(MemberState::RS_PRIMARY, Milliseconds(0)));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit,
                  replCoord->waitForMemberState(MemberState::RS_ARBITER, Milliseconds(0)));
}

TEST_F(ReplCoordTest, WaitForDrainFinish) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    auto replCoord = getReplCoord();
    auto initialTerm = replCoord->getTerm();
    replCoord->setMyLastAppliedOpTime(OpTime(Timestamp(1, 1), 0));
    replCoord->setMyLastDurableOpTime(OpTime(Timestamp(1, 1), 0));
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

    // Single node cluster - this node should start election on setFollowerMode() completion.
    replCoord->waitForElectionFinish_forTest();

    // Successful dry run election increases term.
    ASSERT_EQUALS(initialTerm + 1, replCoord->getTerm());

    auto timeout = Milliseconds(1);
    ASSERT_OK(replCoord->waitForMemberState(MemberState::RS_PRIMARY, timeout));

    ASSERT(replCoord->getApplierState() == ReplicationCoordinator::ApplierState::Draining);
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, replCoord->waitForDrainFinish(timeout));

    ASSERT_EQUALS(ErrorCodes::BadValue, replCoord->waitForDrainFinish(Milliseconds(-1)));

    const auto opCtx = makeOperationContext();
    signalDrainComplete(opCtx.get());
    ASSERT_OK(replCoord->waitForDrainFinish(timeout));

    // Zero timeout is fine.
    ASSERT_OK(replCoord->waitForDrainFinish(Milliseconds(0)));
}

TEST_F(
    ReplCoordTest,
    PopulateUnsetWriteConcernOptionsSyncModeReturnsInputWithSyncModeNoneIfUnsetAndWriteConcernMajorityJournalDefaultIsFalse) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault"
                            << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.wMode = WriteConcernOptions::kMajority;
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
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault"
                            << true),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.wMode = WriteConcernOptions::kMajority;
    wc.syncMode = WriteConcernOptions::SyncMode::UNSET;
    ASSERT(WriteConcernOptions::SyncMode::JOURNAL ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(ReplCoordTest, PopulateUnsetWriteConcernOptionsSyncModeReturnsInputIfSyncModeIsNotUnset) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault"
                            << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.wMode = WriteConcernOptions::kMajority;
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
                            << "version"
                            << 1
                            << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))
                            << "writeConcernMajorityJournalDefault"
                            << false),
                       HostAndPort("test1", 1234));

    WriteConcernOptions wc;
    wc.syncMode = WriteConcernOptions::SyncMode::UNSET;
    wc.wMode = "not the value of kMajority";
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);

    wc.wMode = "like literally anythingelse";
    ASSERT(WriteConcernOptions::SyncMode::NONE ==
           getReplCoord()->populateUnsetWriteConcernOptionsSyncMode(wc).syncMode);
}

TEST_F(ReplCoordTest, NodeStoresElectionVotes) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                                         << "mySet"
                                                         << "term"
                                                         << 7LL
                                                         << "candidateIndex"
                                                         << 2LL
                                                         << "configVersion"
                                                         << 2LL
                                                         << "dryRun"
                                                         << false
                                                         << "lastCommittedOp"
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
                            << "version"
                            << 2
                            << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id"
                                               << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id"
                                                  << 1)
                                          << BSON("host"
                                                  << "node3:12345"
                                                  << "_id"
                                                  << 2))),
                       HostAndPort("node1", 12345));
    auto time = OpTimeWithTermOne(100, 1);
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastAppliedOpTime(time);
    getReplCoord()->setMyLastDurableOpTime(time);
    simulateSuccessfulV1Election();

    auto opCtx = makeOperationContext();

    ReplSetRequestVotesArgs args;
    ASSERT_OK(args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                                         << "mySet"
                                                         << "term"
                                                         << 7LL
                                                         << "candidateIndex"
                                                         << 2LL
                                                         << "configVersion"
                                                         << 2LL
                                                         << "dryRun"
                                                         << true
                                                         << "lastCommittedOp"
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

// TODO(schwerin): Unit test election id updating
}  // namespace
}  // namespace repl
}  // namespace mongo
