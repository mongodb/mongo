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

#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_after_optime_args.h"
#include "mongo/db/repl/read_after_optime_response.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"  // ReplSetReconfigArgs
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
typedef ReplicationCoordinator::ReplSetReconfigArgs ReplSetReconfigArgs;
Status kInterruptedStatus(ErrorCodes::Interrupted, "operation was interrupted");

// Helper class to wrap Timestamp as an OpTime with term 0.
struct OpTimeWithTermZero {
    OpTimeWithTermZero(unsigned int sec, unsigned int i) : timestamp(sec, i) {}
    operator OpTime() const {
        return OpTime(timestamp, 0);
    }

    Timestamp timestamp;
};

TEST_F(ReplCoordTest, StartupWithValidLocalConfig) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
}

TEST_F(ReplCoordTest, StartupWithConfigMissingSelf) {
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
    ASSERT_EQUALS(1, countLogLinesContaining("NodeNotFound"));
}

TEST_F(ReplCoordTest, StartupWithLocalConfigSetNameMismatch) {
    init("mySet");
    startCapturingLogMessages();
    assertStartSuccess(BSON("_id"
                            << "notMySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("reports set name of notMySet,"));
}

TEST_F(ReplCoordTest, StartupWithNoLocalConfig) {
    startCapturingLogMessages();
    start();
    stopCapturingLogMessages();
    ASSERT_EQUALS(2, countLogLinesContaining("Did not find local "));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithEmptyConfig) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateSucceedsWithOneNodeConfig) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Starting uninitialized, show that we can perform the initiate behavior.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

    // Show that initiate fails after it has already succeeded.
    BSONObjBuilder result2;
    ASSERT_EQUALS(
        ErrorCodes::AlreadyInitialized,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result2));

    // Still in repl set mode, even after failed reinitiate.
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateSucceedsAfterFailing) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    // Having failed to initiate once, show that we can now initiate.
    BSONObjBuilder result1;
    ASSERT_OK(
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateFailsIfAlreadyInitialized) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                     << "node1:12345"))),
                       HostAndPort("node1", 12345));
    BSONObjBuilder result;
    ASSERT_EQUALS(
        ErrorCodes::AlreadyInitialized,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 2 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                             << "node1:12345"))),
                                               &result));
}

TEST_F(ReplCoordTest, InitiateFailsIfSelfMissing) {
    OperationContextNoop txn;
    BSONObjBuilder result;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node4"))),
                                               &result));
}

void doReplSetInitiate(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    *status =
        replCoord->processReplSetInitiate(&txn,
                                          BSON("_id"
                                               << "mySet"
                                               << "version" << 1 << "members"
                                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                        << "node1:12345")
                                                             << BSON("_id" << 1 << "host"
                                                                           << "node2:54321"))),
                                          &garbage);
}

TEST_F(ReplCoordTest, InitiateFailsIfQuorumNotMet) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(1);
    hbArgs.setCheckEmpty(true);
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread(stdx::bind(doReplSetInitiate, getReplCoord(), &status));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_EQUALS(hbArgs.toBSON(), noi->getRequest().cmdObj);
    getNet()->scheduleResponse(
        noi, startDate + Milliseconds(10), ResponseStatus(ErrorCodes::NoSuchKey, "No response"));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiatePassesIfQuorumMet) {
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("mySet");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(1);
    hbArgs.setCheckEmpty(true);
    hbArgs.setSenderHost(HostAndPort("node1", 12345));
    hbArgs.setSenderId(0);

    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread prsiThread(stdx::bind(doReplSetInitiate, getReplCoord(), &status));
    const Date_t startDate = getNet()->now();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    ASSERT_EQUALS(HostAndPort("node2", 54321), noi->getRequest().target);
    ASSERT_EQUALS("admin", noi->getRequest().dbname);
    ASSERT_EQUALS(hbArgs.toBSON(), noi->getRequest().cmdObj);
    ReplSetHeartbeatResponse hbResp;
    hbResp.setConfigVersion(0);
    getNet()->scheduleResponse(
        noi,
        startDate + Milliseconds(10),
        ResponseStatus(RemoteCommandResponse(hbResp.toBSON(false), Milliseconds(8))));
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    prsiThread.join();
    ASSERT_OK(status);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, InitiateFailsWithSetNameMismatch) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_EQUALS(
        ErrorCodes::InvalidReplicaSetConfig,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "wrongSet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWithoutReplSetFlag) {
    OperationContextNoop txn;
    init("");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    ASSERT_EQUALS(
        ErrorCodes::NoReplicationEnabled,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, InitiateFailsWhileStoringLocalConfigDocument) {
    OperationContextNoop txn;
    init("mySet");
    start(HostAndPort("node1", 12345));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);

    BSONObjBuilder result1;
    getExternalState()->setStoreLocalConfigDocumentStatus(
        Status(ErrorCodes::OutOfDiskSpace, "The test set this"));
    ASSERT_EQUALS(
        ErrorCodes::OutOfDiskSpace,
        getReplCoord()->processReplSetInitiate(&txn,
                                               BSON("_id"
                                                    << "mySet"
                                                    << "version" << 1 << "members"
                                                    << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                             << "node1:12345"))),
                                               &result1));
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, CheckReplEnabledForCommandNotRepl) {
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

TEST_F(ReplCoordTest, checkReplEnabledForCommandConfigSvr) {
    ReplSettings settings;
    serverGlobalParams.configsvr = true;
    init(settings);
    start();

    // check status NoReplicationEnabled and result mentions configsrv
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NoReplicationEnabled);
    ASSERT_EQUALS(result.obj()["info"].String(), "configsvr");
    serverGlobalParams.configsvr = false;
}

TEST_F(ReplCoordTest, checkReplEnabledForCommandNoConfig) {
    start();

    // check status NotYetInitialized and result mentions rs.initiate
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, ErrorCodes::NotYetInitialized);
    ASSERT_TRUE(result.obj()["info"].String().find("rs.initiate") != std::string::npos);
}

TEST_F(ReplCoordTest, checkReplEnabledForCommandWorking) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    // check status OK and result is empty
    BSONObjBuilder result;
    Status status = getReplCoord()->checkReplEnabledForCommand(&result);
    ASSERT_EQUALS(status, Status::OK());
    ASSERT_TRUE(result.obj().isEmpty());
}

TEST_F(ReplCoordTest, BasicRBIDUsage) {
    start();
    BSONObjBuilder result;
    getReplCoord()->processReplSetGetRBID(&result);
    long long initialValue = result.obj()["rbid"].Int();
    getReplCoord()->incrementRollbackID();

    BSONObjBuilder result2;
    getReplCoord()->processReplSetGetRBID(&result2);
    long long incrementedValue = result2.obj()["rbid"].Int();
    ASSERT_EQUALS(incrementedValue, initialValue + 1);
}

TEST_F(ReplCoordTest, AwaitReplicationNoReplEnabled) {
    init("");
    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 2;

    // Because we didn't set ReplSettings.replSet, it will think we're a standalone so
    // awaitReplication will always work.
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationMasterSlaveMajorityBaseCase) {
    ReplSettings settings;
    settings.master = true;
    init(settings);
    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 2;


    writeConcern.wNumNodes = 0;
    writeConcern.wMode = WriteConcernOptions::kMajority;
    // w:majority always works on master/slave
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationReplSetBaseCases) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));

    OperationContextNoop txn;
    OpTimeWithTermZero time(100, 1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 0;  // Waiting for 0 nodes always works
    writeConcern.wMode = "";

    // Should fail when not primary
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);

    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    statusAndDur = getReplCoord()->awaitReplication(&txn, time, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesNonBlocking) {
    OperationContextNoop txn;
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("host"
                                << "node1:12345"
                                << "_id" << 0)
                           << BSON("host"
                                   << "node2:12345"
                                   << "_id" << 1) << BSON("host"
                                                          << "node3:12345"
                                                          << "_id" << 2) << BSON("host"
                                                                                 << "node4:12345"
                                                                                 << "_id" << 3))),
        HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    // 1 node waiting for time 1
    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastOptime(time1);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time1
    writeConcern.wNumNodes = 2;
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 2 nodes waiting for time2
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    getReplCoord()->setMyLastOptime(time2);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time2));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_OK(statusAndDur.status);

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
    ASSERT_OK(statusAndDur.status);
}

TEST_F(ReplCoordTest, AwaitReplicationNamedModesNonBlocking) {
    OperationContextNoop txn;
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "node0"
                                      << "tags" << BSON("dc"
                                                        << "NA"
                                                        << "rack"
                                                        << "rackNA1"))
                           << BSON("_id" << 1 << "host"
                                         << "node1"
                                         << "tags" << BSON("dc"
                                                           << "NA"
                                                           << "rack"
                                                           << "rackNA2"))
                           << BSON("_id" << 2 << "host"
                                         << "node2"
                                         << "tags" << BSON("dc"
                                                           << "NA"
                                                           << "rack"
                                                           << "rackNA3"))
                           << BSON("_id" << 3 << "host"
                                         << "node3"
                                         << "tags" << BSON("dc"
                                                           << "EU"
                                                           << "rack"
                                                           << "rackEU1"))
                           << BSON("_id" << 4 << "host"
                                         << "node4"
                                         << "tags" << BSON("dc"
                                                           << "EU"
                                                           << "rack"
                                                           << "rackEU2"))) << "settings"
             << BSON("getLastErrorModes" << BSON("multiDC" << BSON("dc" << 2) << "multiDCAndRack"
                                                           << BSON("dc" << 2 << "rack" << 3)))),
        HostAndPort("node0"));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    // Test invalid write concern
    WriteConcernOptions invalidWriteConcern;
    invalidWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    invalidWriteConcern.wMode = "fakemode";

    ReplicationCoordinator::StatusAndDuration statusAndDur =
        getReplCoord()->awaitReplication(&txn, time1, invalidWriteConcern);
    ASSERT_EQUALS(ErrorCodes::UnknownReplWriteConcern, statusAndDur.status);


    // Set up valid write concerns for the rest of the test
    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;

    WriteConcernOptions multiDCWriteConcern;
    multiDCWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiDCWriteConcern.wMode = "multiDC";

    WriteConcernOptions multiRackWriteConcern;
    multiRackWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    multiRackWriteConcern.wMode = "multiDCAndRack";


    // Nothing satisfied
    getReplCoord()->setMyLastOptime(time1);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // Majority satisfied but not either custom mode
    getReplCoord()->setLastOptime_forTest(2, 1, time1);
    getReplCoord()->setLastOptime_forTest(2, 2, time1);

    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);

    // All modes satisfied
    getReplCoord()->setLastOptime_forTest(2, 3, time1);

    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
    ASSERT_OK(statusAndDur.status);

    // multiDC satisfied but not majority or multiRack
    getReplCoord()->setMyLastOptime(time2);
    getReplCoord()->setLastOptime_forTest(2, 3, time2);

    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, majorityWriteConcern);
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiDCWriteConcern);
    ASSERT_OK(statusAndDur.status);
    statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiRackWriteConcern);
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
    ReplicationAwaiter(ReplicationCoordinatorImpl* replCoord, OperationContext* txn)
        : _replCoord(replCoord),
          _finished(false),
          _result(ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0))) {}

    void setOpTime(const OpTime& ot) {
        _optime = ot;
    }

    void setWriteConcern(const WriteConcernOptions& wc) {
        _writeConcern = wc;
    }

    // may block
    ReplicationCoordinator::StatusAndDuration getResult() {
        _thread->join();
        ASSERT(_finished);
        return _result;
    }

    void start(OperationContext* txn) {
        ASSERT(!_finished);
        _thread.reset(
            new stdx::thread(stdx::bind(&ReplicationAwaiter::_awaitReplication, this, txn)));
    }

    void reset() {
        ASSERT(_finished);
        _finished = false;
        _result = ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
    }

private:
    void _awaitReplication(OperationContext* txn) {
        _result = _replCoord->awaitReplication(txn, _optime, _writeConcern);
        _finished = true;
    }

    ReplicationCoordinatorImpl* _replCoord;
    bool _finished;
    OpTime _optime;
    WriteConcernOptions _writeConcern;
    ReplicationCoordinator::StatusAndDuration _result;
    std::unique_ptr<stdx::thread> _thread;
};

TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesBlocking) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time1
    awaiter.setOpTime(time1);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();

    // 3 nodes waiting for time2
    writeConcern.wNumNodes = 3;
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationTimeout) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = 50;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationShutdown) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));
    shutdown();
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationStepDown) {
    // Test that a thread blocked in awaitReplication will be woken up and return NotMaster
    // if the node steps down while it is waiting.
    OperationContextReplMock txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;

    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));
    getReplCoord()->stepDown(&txn, true, Milliseconds(0), Milliseconds(1000));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationInterrupt) {
    // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
    const unsigned int opID = 100;
    OperationContextReplMock txn{opID};
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "node1")
                                          << BSON("_id" << 1 << "host"
                                                        << "node2") << BSON("_id" << 2 << "host"
                                                                                  << "node3"))),
                       HostAndPort("node1"));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    ReplicationAwaiter awaiter(getReplCoord(), &txn);

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 2;


    // 2 nodes waiting for time2
    awaiter.setOpTime(time2);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time1));

    txn.setCheckForInterruptStatus(kInterruptedStatus);
    getReplCoord()->interrupt(opID);
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::Interrupted, statusAndDur.status);
    awaiter.reset();
}

class StepDownTest : public ReplCoordTest {
protected:
    OID myRid;
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
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        myRid = getReplCoord()->getMyRID();
    }
};

TEST_F(ReplCoordTest, UpdateTerm) {
    ReplCoordTest::setUp();
    init("mySet/test1:1234,test2:1234,test3:1234");

    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))
             << "protocolVersion" << 1),
        HostAndPort("test1", 1234));
    getReplCoord()->setMyLastOptime(OpTime(Timestamp(100, 1), 0));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    simulateSuccessfulV1Election();

    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // lower term, no change
    getReplCoord()->updateTerm(0);
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // same term, no change
    getReplCoord()->updateTerm(1);
    ASSERT_EQUALS(1, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // higher term, step down and change term
    Handle cbHandle;
    getReplCoord()->updateTerm_forTest(2);
    ASSERT_EQUALS(2, getReplCoord()->getTerm());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownNotPrimary) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(0));
    ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownTimeoutAcquiringGlobalLock) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulElection();

    // Make sure stepDown cannot grab the global shared lock
    Lock::GlobalWrite lk(txn.lockState());

    Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000));
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(StepDownTest, StepDownNoWaiting) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    // All nodes are caught up
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    simulateSuccessfulElection();

    enterNetwork();
    getNet()->runUntil(getNet()->now() + Seconds(2));
    ASSERT(getNet()->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgs hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        hbResp.setOpTime(optime1);
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


    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT_OK(getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000)));
    enterNetwork();  // So we can safely inspect the topology coordinator
    ASSERT_EQUALS(getNet()->now() + Seconds(1), getTopoCoord().getStepDownTime());
    ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
    exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(ReplCoordTest, StepDownAndBackUpSingleNode) {
    init("mySet");

    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test1:1234"))),
                       HostAndPort("test1", 1234));
    OperationContextReplMock txn;
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);

    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    ASSERT_OK(getReplCoord()->stepDown(&txn, true, Milliseconds(0), Milliseconds(1000)));
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

/**
 * Used to run wait for stepDown() to finish in a separate thread without blocking execution of
 * the test. To use, set the values of "force", "waitTime", and "stepDownTime", which will be
 * used as the arguments passed to stepDown, and then call
 * start(), which will spawn a thread that calls stepDown.  No calls may be made
 * on the StepDownRunner instance between calling start and getResult().  After returning
 * from getResult(), you can call reset() to allow the StepDownRunner to be reused for another
 * stepDown call.
 */
class StepDownRunner {
public:
    StepDownRunner(ReplicationCoordinatorImpl* replCoord)
        : _replCoord(replCoord),
          _finished(false),
          _result(Status::OK()),
          _force(false),
          _waitTime(0),
          _stepDownTime(0) {}

    // may block
    Status getResult() {
        _thread->join();
        ASSERT(_finished);
        return _result;
    }

    void start(OperationContext* txn) {
        ASSERT(!_finished);
        _thread.reset(new stdx::thread(stdx::bind(&StepDownRunner::_stepDown, this, txn)));
    }

    void reset() {
        ASSERT(_finished);
        _finished = false;
        _result = Status(ErrorCodes::InternalError, "Result Status never set");
    }

    void setForce(bool force) {
        _force = force;
    }

    void setWaitTime(const Milliseconds& waitTime) {
        _waitTime = waitTime;
    }

    void setStepDownTime(const Milliseconds& stepDownTime) {
        _stepDownTime = stepDownTime;
    }

private:
    void _stepDown(OperationContext* txn) {
        _result = _replCoord->stepDown(txn, _force, _waitTime, _stepDownTime);
        _finished = true;
    }

    ReplicationCoordinatorImpl* _replCoord;
    bool _finished;
    Status _result;
    std::unique_ptr<stdx::thread> _thread;
    bool _force;
    Milliseconds _waitTime;
    Milliseconds _stepDownTime;
};

TEST_F(StepDownTest, StepDownNotCaughtUp) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    getReplCoord()->setMyLastOptime(optime2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    // Try to stepDown but time out because no secondaries are caught up
    StepDownRunner runner(getReplCoord());
    runner.setForce(false);
    runner.setWaitTime(Milliseconds(0));
    runner.setStepDownTime(Milliseconds(1000));

    simulateSuccessfulElection();

    runner.start(&txn);
    Status status = runner.getResult();
    ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    // Now use "force" to force it to step down even though no one is caught up
    runner.reset();
    getNet()->enterNetwork();
    const Date_t startDate = getNet()->now();
    while (startDate + Milliseconds(1000) < getNet()->now()) {
        while (getNet()->hasReadyRequests()) {
            getNet()->blackHole(getNet()->getNextReadyRequest());
        }
        getNet()->runUntil(startDate + Milliseconds(1000));
    }
    getNet()->exitNetwork();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
    runner.setForce(true);
    runner.start(&txn);
    status = runner.getResult();
    ASSERT_OK(status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, StepDownCatchUp) {
    OperationContextReplMock txn;
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    getReplCoord()->setMyLastOptime(optime2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    // stepDown where the secondary actually has to catch up before the stepDown can succeed
    StepDownRunner runner(getReplCoord());
    runner.setForce(false);
    runner.setWaitTime(Milliseconds(10000));
    runner.setStepDownTime(Milliseconds(60000));

    simulateSuccessfulElection();

    runner.start(&txn);

    // Make a secondary actually catch up
    enterNetwork();
    getNet()->runUntil(getNet()->now() + Milliseconds(2000));
    ASSERT(getNet()->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
    RemoteCommandRequest request = noi->getRequest();
    log() << request.target.toString() << " processing " << request.cmdObj;
    ReplSetHeartbeatArgs hbArgs;
    if (hbArgs.initialize(request.cmdObj).isOK()) {
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName(hbArgs.getSetName());
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setConfigVersion(hbArgs.getConfigVersion());
        hbResp.setOpTime(optime2);
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

    ASSERT_OK(runner.getResult());
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
}

TEST_F(StepDownTest, InterruptStepDown) {
    const unsigned int opID = 100;
    OperationContextReplMock txn{opID};
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    // No secondary is caught up
    getReplCoord()->setMyLastOptime(optime2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime1));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime1));

    // stepDown where the secondary actually has to catch up before the stepDown can succeed
    StepDownRunner runner(getReplCoord());
    runner.setForce(false);
    runner.setWaitTime(Milliseconds(10000));
    runner.setStepDownTime(Milliseconds(60000));

    simulateSuccessfulElection();
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    runner.start(&txn);

    txn.setCheckForInterruptStatus(kInterruptedStatus);
    getReplCoord()->interrupt(opID);

    ASSERT_EQUALS(ErrorCodes::Interrupted, runner.getResult());
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());
}

TEST_F(ReplCoordTest, GetReplicationModeNone) {
    init();
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

TEST_F(ReplCoordTest, GetReplicationModeMaster) {
    // modeMasterSlave if master set
    ReplSettings settings;
    settings.master = true;
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, GetReplicationModeSlave) {
    // modeMasterSlave if the slave flag was set
    ReplSettings settings;
    settings.slave = SimpleSlave;
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave, getReplCoord()->getReplicationMode());
}

TEST_F(ReplCoordTest, GetReplicationModeRepl) {
    // modeReplSet if the set name was supplied.
    ReplSettings settings;
    settings.replSet = "mySet/node1:12345";
    init(settings);
    ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));
}

TEST_F(ReplCoordTest, TestPrepareReplSetUpdatePositionCommand) {
    OperationContextNoop txn;
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))),
        HostAndPort("test1", 1234));
    OpTimeWithTermZero optime1(100, 1);
    OpTimeWithTermZero optime2(100, 2);
    OpTimeWithTermZero optime3(2, 1);
    getReplCoord()->setMyLastOptime(optime1);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 1, optime2));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(1, 2, optime3));

    // Check that the proper BSON is generated for the replSetUpdatePositionCommand
    BSONObjBuilder cmdBuilder;
    getReplCoord()->prepareReplSetUpdatePositionCommand(&cmdBuilder);
    BSONObj cmd = cmdBuilder.done();

    ASSERT_EQUALS(2, cmd.nFields());
    ASSERT_EQUALS("replSetUpdatePosition", cmd.firstElement().fieldNameStringData());

    std::set<long long> memberIds;
    BSONForEach(entryElement, cmd["optimes"].Obj()) {
        BSONObj entry = entryElement.Obj();
        long long memberId = entry["memberId"].Number();
        memberIds.insert(memberId);
        if (memberId == 0) {
            // TODO(siyuan) Update when we change replSetUpdatePosition format
            ASSERT_EQUALS(optime1.timestamp, entry["optime"].timestamp());
        } else if (memberId == 1) {
            ASSERT_EQUALS(optime2.timestamp, entry["optime"].timestamp());
        } else {
            ASSERT_EQUALS(2, memberId);
            ASSERT_EQUALS(optime3.timestamp, entry["optime"].timestamp());
        }
    }
    ASSERT_EQUALS(3U, memberIds.size());  // Make sure we saw all 3 nodes
}

TEST_F(ReplCoordTest, SetMaintenanceMode) {
    init("mySet/test1:1234,test2:1234,test3:1234");
    assertStartSuccess(
        BSON("_id"
             << "mySet"
             << "version" << 1 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                      << "test1:1234")
                           << BSON("_id" << 1 << "host"
                                         << "test2:1234") << BSON("_id" << 2 << "host"
                                                                        << "test3:1234"))),
        HostAndPort("test2", 1234));
    OperationContextNoop txn;
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));

    // Can't unset maintenance mode if it was never set to begin with.
    Status status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // valid set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // If we go into rollback while in maintenance mode, our state changes to RS_ROLLBACK.
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // When we go back to SECONDARY, we still observe RECOVERING because of maintenance mode.
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());

    // Can set multiple times
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));

    // Need to unset the number of times you set
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    status = getReplCoord()->setMaintenanceMode(false);
    // fourth one fails b/c we only set three times
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    // Unsetting maintenance mode changes our state to secondary if maintenance mode was
    // the only thinking keeping us out of it.
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // From rollback, entering and exiting maintenance mode doesn't change perceived
    // state.
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());

    // Rollback is sticky even if entered while in maintenance mode.
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_TRUE(getReplCoord()->getMemberState().recovering());
    getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
    ASSERT_TRUE(getReplCoord()->getMemberState().rollback());
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

    // Can't modify maintenance mode when PRIMARY
    simulateSuccessfulElection();

    status = getReplCoord()->setMaintenanceMode(true);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
    ASSERT_TRUE(getReplCoord()->getMemberState().primary());

    simulateStepDownOnIsolation();

    status = getReplCoord()->setMaintenanceMode(false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_OK(getReplCoord()->setMaintenanceMode(true));
    ASSERT_OK(getReplCoord()->setMaintenanceMode(false));
}

TEST_F(ReplCoordTest, GetHostsWrittenToReplSet) {
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
    OperationContextNoop txn;

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time1));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(myHost, caughtUpHosts[0]);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time2));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(2U, caughtUpHosts.size());
    if (myHost == caughtUpHosts[0]) {
        ASSERT_EQUALS(client2Host, caughtUpHosts[1]);
    } else {
        ASSERT_EQUALS(client2Host, caughtUpHosts[0]);
        ASSERT_EQUALS(myHost, caughtUpHosts[1]);
    }
}

TEST_F(ReplCoordTest, GetHostsWrittenToMasterSlave) {
    ReplSettings settings;
    settings.master = true;
    init(settings);
    HostAndPort clientHost("node2:12345");
    OperationContextNoop txn;

    OID client = OID::gen();
    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);

    getExternalState()->setClientHostAndPort(clientHost);
    HandshakeArgs handshake;
    ASSERT_OK(handshake.initialize(BSON("handshake" << client)));
    ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake));

    getReplCoord()->setMyLastOptime(time2);
    ASSERT_OK(getReplCoord()->setLastOptimeForSlave(client, time1.timestamp));

    std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(0U, caughtUpHosts.size());  // self doesn't get included in master-slave

    ASSERT_OK(getReplCoord()->setLastOptimeForSlave(client, time2.timestamp));
    caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
    ASSERT_EQUALS(1U, caughtUpHosts.size());
    ASSERT_EQUALS(clientHost, caughtUpHosts[0]);
}

TEST_F(ReplCoordTest, GetOtherNodesInReplSetNoConfig) {
    start();
    ASSERT_EQUALS(0U, getReplCoord()->getOtherNodesInReplSet().size());
}

TEST_F(ReplCoordTest, GetOtherNodesInReplSet) {
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "h1")
                                          << BSON("_id" << 1 << "host"
                                                        << "h2")
                                          << BSON("_id" << 2 << "host"
                                                        << "h3"
                                                        << "priority" << 0 << "hidden" << true))),
                       HostAndPort("h1"));

    std::vector<HostAndPort> otherNodes = getReplCoord()->getOtherNodesInReplSet();
    ASSERT_EQUALS(2U, otherNodes.size());
    if (otherNodes[0] == HostAndPort("h2")) {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[1]);
    } else {
        ASSERT_EQUALS(HostAndPort("h3"), otherNodes[0]);
        ASSERT_EQUALS(HostAndPort("h2"), otherNodes[0]);
    }
}

TEST_F(ReplCoordTest, IsMasterNoConfig) {
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
        BSON("_id"
             << "mySet"
             << "version" << 2 << "members"
             << BSON_ARRAY(BSON("_id" << 0 << "host" << h1.toString())
                           << BSON("_id" << 1 << "host" << h2.toString())
                           << BSON("_id" << 2 << "host" << h3.toString() << "arbiterOnly" << true)
                           << BSON("_id" << 3 << "host" << h4.toString() << "priority" << 0
                                         << "tags" << BSON("key1"
                                                           << "value1"
                                                           << "key2"
                                                           << "value2")))),
        h4);
    getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(getReplCoord()->getMemberState().secondary());

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

    unordered_map<std::string, std::string> tags = response.getTags();
    ASSERT_EQUALS(2U, tags.size());
    ASSERT_EQUALS("value1", tags["key1"]);
    ASSERT_EQUALS("value2", tags["key2"]);

    IsMasterResponse roundTripped;
    ASSERT_OK(roundTripped.initialize(response.toBSON()));
}

TEST_F(ReplCoordTest, ShutDownBeforeStartUpFinished) {
    init();
    startCapturingLogMessages();
    getReplCoord()->shutdown();
    stopCapturingLogMessages();
    ASSERT_EQUALS(1,
                  countLogLinesContaining("shutdown() called before startReplication() finished"));
}

TEST_F(ReplCoordTest, UpdatePositionWithConfigVersionAndMemberIdTest) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    simulateSuccessfulElection();

    OpTimeWithTermZero time1(100, 1);
    OpTimeWithTermZero time2(100, 2);
    OpTimeWithTermZero staleTime(10, 0);
    getReplCoord()->setMyLastOptime(time1);

    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern.wNumNodes = 1;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition containing ourself, should not process the update for self
    UpdatePositionArgs args;
    ASSERT_OK(args.initialize(BSON("replSetUpdatePosition"
                                   << 1 << "optimes"
                                   << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 0 << "optime"
                                                               << time2.timestamp)))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition with incorrect config version
    UpdatePositionArgs args2;
    ASSERT_OK(args2.initialize(BSON("replSetUpdatePosition"
                                    << 1 << "optimes"
                                    << BSON_ARRAY(BSON("cfgver" << 3 << "memberId" << 1 << "optime"
                                                                << time2.timestamp)))));

    long long cfgver;
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                  getReplCoord()->processReplSetUpdatePosition(args2, &cfgver));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive updatePosition with nonexistent member id
    UpdatePositionArgs args3;
    ASSERT_OK(args3.initialize(BSON("replSetUpdatePosition"
                                    << 1 << "optimes"
                                    << BSON_ARRAY(BSON("cfgver" << 2 << "memberId" << 9 << "optime"
                                                                << time2.timestamp)))));

    ASSERT_EQUALS(ErrorCodes::NodeNotFound, getReplCoord()->processReplSetUpdatePosition(args3, 0));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    // receive a good update position
    getReplCoord()->setMyLastOptime(time2);
    UpdatePositionArgs args4;
    ASSERT_OK(args4.initialize(
        BSON("replSetUpdatePosition"
             << 1 << "optimes"
             << BSON_ARRAY(
                    BSON("cfgver" << 2 << "memberId" << 1 << "optime" << time2.timestamp)
                    << BSON("cfgver" << 2 << "memberId" << 2 << "optime" << time2.timestamp)))));

    ASSERT_OK(getReplCoord()->processReplSetUpdatePosition(args4, 0));
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);

    writeConcern.wNumNodes = 3;
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time2, writeConcern).status);
}

void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345"
                                                      << "priority" << 3)
                                           << BSON("_id" << 1 << "host"
                                                         << "node2:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(&txn, args, &garbage);
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigSimple) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 2));
    simulateSuccessfulElection();

    OpTimeWithTermZero time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // reconfig
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

    NetworkInterfaceMock* net = getNet();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    repl::ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(2);
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
    reconfigThread.join();
    ASSERT_OK(status);

    // satisfy write concern
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 0, time));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 1, time));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(3, 2, time));
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

void doReplSetReconfigToFewer(ReplicationCoordinatorImpl* replCoord, Status* status) {
    OperationContextNoop txn;
    BSONObjBuilder garbage;
    ReplSetReconfigArgs args;
    args.force = false;
    args.newConfigObj = BSON("_id"
                             << "mySet"
                             << "version" << 3 << "members"
                             << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                      << "node1:12345")
                                           << BSON("_id" << 2 << "host"
                                                         << "node3:12345")));
    *status = replCoord->processReplSetReconfig(&txn, args, &garbage);
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigNodeCountExceedsNumberOfNodes) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 2));
    simulateSuccessfulElection();

    OpTimeWithTermZero time(100, 2);

    // 3 nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wNumNodes = 3;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // reconfig to fewer nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfigToFewer, getReplCoord(), &status));

    NetworkInterfaceMock* net = getNet();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    repl::ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(2);
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
    reconfigThread.join();
    ASSERT_OK(status);
    std::cout << "asdf" << std::endl;

    // writeconcern feasability should be reevaluated and an error should be returned
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_EQUALS(ErrorCodes::CannotSatisfyWriteConcern, statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationReconfigToSmallerMajority) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3) << BSON("host"
                                                                         << "node5:12345"
                                                                         << "_id" << 4))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 1));
    simulateSuccessfulElection();

    OpTimeWithTermZero time(100, 2);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time));


    // majority nodes waiting for time
    WriteConcernOptions writeConcern;
    writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
    writeConcern.wMode = WriteConcernOptions::kMajority;

    ReplicationAwaiter awaiter(getReplCoord(), &txn);
    awaiter.setOpTime(time);
    awaiter.setWriteConcern(writeConcern);
    awaiter.start(&txn);

    // demonstrate that majority cannot currently be satisfied
    WriteConcernOptions writeConcern2;
    writeConcern2.wTimeout = WriteConcernOptions::kNoWaiting;
    writeConcern2.wMode = WriteConcernOptions::kMajority;
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, writeConcern2).status);

    // reconfig to three nodes
    Status status(ErrorCodes::InternalError, "Not Set");
    stdx::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

    NetworkInterfaceMock* net = getNet();
    getNet()->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    repl::ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName("mySet");
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(2);
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
    reconfigThread.join();
    ASSERT_OK(status);

    // writeconcern feasability should be reevaluated and be satisfied
    ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
    ASSERT_OK(statusAndDur.status);
    awaiter.reset();
}

TEST_F(ReplCoordTest, AwaitReplicationMajority) {
    // Test that we can satisfy majority write concern can only be
    // statisfied by voting data-bearing members.
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTimeWithTermZero time(100, 0);
    getReplCoord()->setMyLastOptime(time);
    simulateSuccessfulElection();

    WriteConcernOptions majorityWriteConcern;
    majorityWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
    majorityWriteConcern.wMode = WriteConcernOptions::kMajority;

    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    // this member does not vote and as a result should not count towards write concern
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time));
    ASSERT_EQUALS(ErrorCodes::WriteConcernFailed,
                  getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time));
    ASSERT_OK(getReplCoord()->awaitReplication(&txn, time, majorityWriteConcern).status);
}

TEST_F(ReplCoordTest, LastCommittedOpTime) {
    // Test that the commit level advances properly.
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members"
                            << BSON_ARRAY(BSON("host"
                                               << "node1:12345"
                                               << "_id" << 0)
                                          << BSON("host"
                                                  << "node2:12345"
                                                  << "_id" << 1) << BSON("host"
                                                                         << "node3:12345"
                                                                         << "_id" << 2)
                                          << BSON("host"
                                                  << "node4:12345"
                                                  << "_id" << 3 << "votes" << 0)
                                          << BSON("host"
                                                  << "node5:12345"
                                                  << "_id" << 4 << "arbiterOnly" << true))),
                       HostAndPort("node1", 12345));
    ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    OpTimeWithTermZero zero(0, 0);
    OpTimeWithTermZero time(100, 0);
    getReplCoord()->setMyLastOptime(time);
    simulateSuccessfulElection();

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, time));
    ASSERT_EQUALS((OpTime)zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, time));
    ASSERT_EQUALS((OpTime)zero, getReplCoord()->getLastCommittedOpTime());

    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, time));
    ASSERT_EQUALS((OpTime)time, getReplCoord()->getLastCommittedOpTime());


    // Set a new, later OpTime.
    OpTimeWithTermZero newTime = OpTimeWithTermZero(100, 1);
    getReplCoord()->setMyLastOptime(newTime);
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 3, newTime));
    ASSERT_EQUALS((OpTime)time, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 2, newTime));
    // Reached majority of voting nodes with newTime.
    ASSERT_EQUALS((OpTime)newTime, getReplCoord()->getLastCommittedOpTime());
    ASSERT_OK(getReplCoord()->setLastOptime_forTest(2, 1, newTime));
    ASSERT_EQUALS((OpTime)newTime, getReplCoord()->getLastCommittedOpTime());
}

TEST_F(ReplCoordTest, CantUseReadAfterIfNotReplSet) {
    init(ReplSettings());
    OperationContextNoop txn;
    auto result =
        getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(OpTimeWithTermZero(50, 0)));

    ASSERT_FALSE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::NotAReplicaSet, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterWhileShutdown) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(10, 0));

    shutdown();

    auto result =
        getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(OpTimeWithTermZero(50, 0)));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterInterrupted) {
    OperationContextReplMock txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(10, 0));

    txn.setCheckForInterruptStatus(Status(ErrorCodes::Interrupted, "test"));

    auto result =
        getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(OpTimeWithTermZero(50, 0)));

    ASSERT_TRUE(result.didWait());
    ASSERT_EQUALS(ErrorCodes::Interrupted, result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterNoOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    auto result = getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs());

    ASSERT_FALSE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterGreaterOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(100, 0));
    auto result =
        getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(OpTimeWithTermZero(50, 0)));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterEqualOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));


    OpTimeWithTermZero time(100, 0);
    getReplCoord()->setMyLastOptime(time);
    auto result = getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(time));

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterDeferredGreaterOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(0, 0));

    auto pseudoLogOp = std::async(std::launch::async,
                                  [this]() {
                                      // Not guaranteed to be scheduled after waitUnitl blocks...
                                      getReplCoord()->setMyLastOptime(OpTimeWithTermZero(200, 0));
                                  });

    auto result =
        getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(OpTimeWithTermZero(100, 0)));
    pseudoLogOp.get();

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

TEST_F(ReplCoordTest, ReadAfterDeferredEqualOpTime) {
    OperationContextNoop txn;
    assertStartSuccess(BSON("_id"
                            << "mySet"
                            << "version" << 2 << "members" << BSON_ARRAY(BSON("host"
                                                                              << "node1:12345"
                                                                              << "_id" << 0))),
                       HostAndPort("node1", 12345));

    getReplCoord()->setMyLastOptime(OpTimeWithTermZero(0, 0));

    OpTimeWithTermZero opTimeToWait(100, 0);

    auto pseudoLogOp = std::async(std::launch::async,
                                  [this, &opTimeToWait]() {
                                      // Not guaranteed to be scheduled after waitUnitl blocks...
                                      getReplCoord()->setMyLastOptime(opTimeToWait);
                                  });

    auto result = getReplCoord()->waitUntilOpTime(&txn, ReadAfterOpTimeArgs(opTimeToWait));
    pseudoLogOp.get();

    ASSERT_TRUE(result.didWait());
    ASSERT_OK(result.getStatus());
}

// TODO(schwerin): Unit test election id updating

}  // namespace
}  // namespace repl
}  // namespace mongo
