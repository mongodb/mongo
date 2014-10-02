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

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>
#include <memory>
#include <set>
#include <vector>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_test_fixture.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

    TEST_F(ReplCoordTest, StartupWithValidLocalConfig) {
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                HostAndPort("node1", 12345));
    }

    TEST_F(ReplCoordTest, StartupWithInvalidLocalConfig) {
        startCapturingLogMessages();
        assertStart(ReplicationCoordinator::modeNone,
                    BSON("_id" << "mySet"), HostAndPort("node1", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("configuration does not parse"));
    }

    TEST_F(ReplCoordTest, StartupWithConfigMissingSelf) {
        startCapturingLogMessages();
        assertStart(
                ReplicationCoordinator::modeNone,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                             BSON("_id" << 2 << "host" << "node2:54321"))),
                HostAndPort("node3", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("NodeNotFound"));
    }

    TEST_F(ReplCoordTest, StartupWithLocalConfigSetNameMismatch) {
        init("mySet");
        startCapturingLogMessages();
        assertStart(ReplicationCoordinator::modeNone,
                    BSON("_id" << "notMySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                    HostAndPort("node1", 12345));
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("reports set name of notMySet,"));
    }

    TEST_F(ReplCoordTest, StartupWithNoLocalConfig) {
        startCapturingLogMessages();
        start();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("Did not find local "));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsWithEmptyConfig) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                      getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateSucceedsWithOneNodeConfig) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        // Starting uninitialized, show that we can perform the initiate behavior.
        BSONObjBuilder result1;
        ASSERT_OK(getReplCoord()->processReplSetInitiate(
                          &txn,
                          BSON("_id" << "mySet" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 0 << "host" << "node1:12345"))),
                          &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());

        // Show that initiate fails after it has already succeeded.
        BSONObjBuilder result2;
        ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 1 <<
                                   "members" << BSON_ARRAY(
                                           BSON("_id" << 0 << "host" << "node1:12345"))),
                              &result2));

        // Still in repl set mode, even after failed reinitiate.
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateSucceedsAfterFailing) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                      getReplCoord()->processReplSetInitiate(&txn, BSONObj(), &result));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        // Having failed to initiate once, show that we can now initiate.
        BSONObjBuilder result1;
        ASSERT_OK(getReplCoord()->processReplSetInitiate(
                          &txn,
                          BSON("_id" << "mySet" <<
                               "version" << 1 <<
                               "members" << BSON_ARRAY(
                                       BSON("_id" << 0 << "host" << "node1:12345"))),
                          &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsIfAlreadyInitialized) {
        OperationContextNoop txn;
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
                HostAndPort("node1", 12345));
        BSONObjBuilder result;
        ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 2 <<
                                   "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                                "host" << "node1:12345"))),
                              &result));
    }

    TEST_F(ReplCoordTest, InitiateFailsIfSelfMissing) {
        OperationContextNoop txn;
        BSONObjBuilder result;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ErrorCodes::NodeNotFound,
                      getReplCoord()->processReplSetInitiate(
                              &txn,
                              BSON("_id" << "mySet" <<
                                   "version" << 1 <<
                                   "members" << BSON_ARRAY(
                                           BSON("_id" << 0 << "host" << "node4"))),
                              &result));
    }

    TEST_F(ReplCoordTest, InitiateFailsIfQuorumNotMet) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        BSONObjBuilder result1;
        ASSERT_EQUALS(
                ErrorCodes::NodeNotFound,
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "mySet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345") <<
                                     BSON("_id" << 1 << "host" << "node2:54321"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        ReplSetHeartbeatArgs hbArgs;
        hbArgs.setSetName("mySet");
        hbArgs.setProtocolVersion(1);
        hbArgs.setConfigVersion(1);
        hbArgs.setCheckEmpty(true);
        hbArgs.setSenderHost(HostAndPort("node1", 12345));
        hbArgs.setSenderId(0);
        getNetWithMap()->addResponse(
                ReplicationExecutor::RemoteCommandRequest(
                        HostAndPort("node2", 54321),
                        "admin",
                        hbArgs.toBSON()),
                StatusWith<BSONObj>(BSON("ok" << 1)));

        ASSERT_OK(
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "mySet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345") <<
                                     BSON("_id" << 1 << "host" << "node2:54321"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, InitiateFailsWithSetNameMismatch) {
        OperationContextNoop txn;
        init("mySet");
        start(HostAndPort("node1", 12345));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());

        BSONObjBuilder result1;
        ASSERT_EQUALS(
                ErrorCodes::BadValue,
                getReplCoord()->processReplSetInitiate(
                        &txn,
                        BSON("_id" << "wrongSet" <<
                             "version" << 1 <<
                             "members" << BSON_ARRAY(
                                     BSON("_id" << 0 << "host" << "node1:12345"))),
                        &result1));
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
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
        assertStartSuccess(BSON("_id" << "mySet" <<
                                "version" << 2 <<
                                "members" << BSON_ARRAY(BSON("host" << "node1:12345" <<
                                                             "_id" << 0 ))),
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
        OpTime time(1, 1);

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
        OpTime time(1, 1);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 2;


        writeConcern.wNumNodes = 0;
        writeConcern.wMode = "majority";
        // w:majority always works on master/slave
        ReplicationCoordinator::StatusAndDuration statusAndDur = getReplCoord()->awaitReplication(
                &txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    TEST_F(ReplCoordTest, AwaitReplicationReplSetBaseCases) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2))),
                HostAndPort("node1", 12345));

        OperationContextNoop txn;
        OpTime time(1, 1);

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 0; // Waiting for 0 nodes always works
        writeConcern.wMode = "";

        // Should fail when not primary
        ReplicationCoordinator::StatusAndDuration statusAndDur = getReplCoord()->awaitReplication(
                &txn, time, writeConcern);
        ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);

        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesNonBlocking) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2) <<
                                             BSON("host" << "node4:12345" << "_id" << 3))),
                HostAndPort("node1", 12345));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        OID myOID = getReplCoord()->getMyRID();
        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OID client3 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));
        HandshakeArgs handshake3;
        ASSERT_OK(handshake3.initialize(BSON("handshake" << client3 << "member" << 3)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake3));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        writeConcern.wNumNodes = 1;

        // 1 node waiting for time 1
        ReplicationCoordinator::StatusAndDuration statusAndDur =
                                        getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, myOID, time1));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 2 nodes waiting for time1
        writeConcern.wNumNodes = 2;
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 2 nodes waiting for time2
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, myOID, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client3, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_OK(statusAndDur.status);

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time2));
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, writeConcern);
        ASSERT_OK(statusAndDur.status);
    }

    TEST_F(ReplCoordTest, AwaitReplicationNamedModesNonBlocking) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                  "host" << "node0" <<
                                                  "tags" << BSON("dc" << "NA" <<
                                                                 "rack" << "rackNA1")) <<
                                             BSON("_id" << 1 <<
                                                  "host" << "node1" <<
                                                  "tags" << BSON("dc" << "NA" <<
                                                                 "rack" << "rackNA2")) <<
                                             BSON("_id" << 2 <<
                                                  "host" << "node2" <<
                                                  "tags" << BSON("dc" << "NA" <<
                                                                 "rack" << "rackNA3")) <<
                                             BSON("_id" << 3 <<
                                                  "host" << "node3" <<
                                                  "tags" << BSON("dc" << "EU" <<
                                                                 "rack" << "rackEU1")) <<
                                             BSON("_id" << 4 <<
                                                  "host" << "node4" <<
                                                  "tags" << BSON("dc" << "EU" <<
                                                                 "rack" << "rackEU2"))) <<
                     "settings" << BSON("getLastErrorModes" <<
                                        BSON("multiDC" << BSON("dc" << 2) <<
                                             "multiDCAndRack" << BSON("dc" << 2 << "rack" << 3)))),
                HostAndPort("node0"));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        OID selfRID = getReplCoord()->getMyRID();
        OID clientRID1 = OID::gen();
        OID clientRID2 = OID::gen();
        OID clientRID3 = OID::gen();
        OID clientRID4 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << clientRID1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << clientRID2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));
        HandshakeArgs handshake3;
        ASSERT_OK(handshake3.initialize(BSON("handshake" << clientRID3 << "member" << 3)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake3));
        HandshakeArgs handshake4;
        ASSERT_OK(handshake4.initialize(BSON("handshake" << clientRID4 << "member" << 4)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake4));

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
        majorityWriteConcern.wMode = "majority";

        WriteConcernOptions multiDCWriteConcern;
        multiDCWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        multiDCWriteConcern.wMode = "multiDC";

        WriteConcernOptions multiRackWriteConcern;
        multiRackWriteConcern.wTimeout = WriteConcernOptions::kNoWaiting;
        multiRackWriteConcern.wMode = "multiDCAndRack";


        // Nothing satisfied
        getReplCoord()->setLastOptime(&txn, selfRID, time1);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);

        // Majority satisfied but not either custom mode
        getReplCoord()->setLastOptime(&txn, clientRID1, time1);
        getReplCoord()->setLastOptime(&txn, clientRID2, time1);

        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
        ASSERT_OK(statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);

        // All modes satisfied
        getReplCoord()->setLastOptime(&txn, clientRID3, time1);

        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, majorityWriteConcern);
        ASSERT_OK(statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiDCWriteConcern);
        ASSERT_OK(statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time1, multiRackWriteConcern);
        ASSERT_OK(statusAndDur.status);

        // multiDC satisfied but not majority or multiRack
        getReplCoord()->setLastOptime(&txn, selfRID, time2);
        getReplCoord()->setLastOptime(&txn, clientRID3, time2);

        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, majorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiDCWriteConcern);
        ASSERT_OK(statusAndDur.status);
        statusAndDur = getReplCoord()->awaitReplication(&txn, time2, multiRackWriteConcern);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
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

        ReplicationAwaiter(ReplicationCoordinatorImpl* replCoord, OperationContext* txn) :
            _replCoord(replCoord), _finished(false),
            _result(ReplicationCoordinator::StatusAndDuration(
                    Status::OK(), ReplicationCoordinator::Milliseconds(0))) {}

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
            _thread.reset(new boost::thread(stdx::bind(&ReplicationAwaiter::_awaitReplication,
                                                       this,
                                                       txn)));
        }

        void reset() {
            ASSERT(_finished);
            _finished = false;
            _result = ReplicationCoordinator::StatusAndDuration(
                    Status::OK(), ReplicationCoordinator::Milliseconds(0));
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
        boost::scoped_ptr<boost::thread> _thread;
    };

    TEST_F(ReplCoordTest, AwaitReplicationNumberOfNodesBlocking) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2))),
                HostAndPort("node1", 12345));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID selfRID = getReplCoord()->getMyRID();
        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time1
        awaiter.setOpTime(time1);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, selfRID, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, selfRID, time2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();

        // 3 nodes waiting for time2
        writeConcern.wNumNodes = 3;
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time2));
        statusAndDur = awaiter.getResult();
        ASSERT_OK(statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationTimeout) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2))),
                HostAndPort("node1", 12345));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID selfRID = getReplCoord()->getMyRID();
        OID client = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake;
        ASSERT_OK(handshake.initialize(BSON("handshake" << client << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = 50;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, selfRID, time2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client, time1));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationShutdown) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2))),
                HostAndPort("node1", 12345));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        shutdown();
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, statusAndDur.status);
        awaiter.reset();
    }

    TEST_F(ReplCoordTest, AwaitReplicationStepDown) {
        // Test that a thread blocked in awaitReplication will be woken up and return NotMaster
        // if the node steps down while it is waiting.
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0) <<
                                             BSON("host" << "node2:12345" << "_id" << 1) <<
                                             BSON("host" << "node3:12345" << "_id" << 2))),
                HostAndPort("node1", 12345));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoop txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));
        getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000));
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::NotMaster, statusAndDur.status);
        awaiter.reset();
    }

    class OperationContextNoopWithInterrupt : public OperationContextNoop {
    public:

        OperationContextNoopWithInterrupt() : _opID(0), _interruptOp(false) {}

        virtual unsigned int getOpID() const {
            return _opID;
        }

        /**
         * Can only be called before any multi-threaded access to this object has begun.
         */
        void setOpID(unsigned int opID) {
            _opID = opID;
        }

        virtual void checkForInterrupt(bool heedMutex = true) const {
            if (_interruptOp) {
                uasserted(ErrorCodes::Interrupted, "operation was interrupted");
            }
        }

        /**
         * Can only be called before any multi-threaded access to this object has begun.
         */
        void setInterruptOp(bool interrupt) {
            _interruptOp = interrupt;
        }

    private:
        unsigned int _opID;
        bool _interruptOp;
    };

    TEST_F(ReplCoordTest, AwaitReplicationInterrupt) {
        // Tests that a thread blocked in awaitReplication can be killed by a killOp operation
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "node1") <<
                                             BSON("_id" << 1 << "host" << "node2") <<
                                             BSON("_id" << 2 << "host" << "node3"))),
                HostAndPort("node1"));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        OperationContextNoopWithInterrupt txn;
        ReplicationAwaiter awaiter(getReplCoord(), &txn);

        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        WriteConcernOptions writeConcern;
        writeConcern.wTimeout = WriteConcernOptions::kNoTimeout;
        writeConcern.wNumNodes = 2;

        unsigned int opID = 100;
        txn.setOpID(opID);

        // 2 nodes waiting for time2
        awaiter.setOpTime(time2);
        awaiter.setWriteConcern(writeConcern);
        awaiter.start(&txn);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time1));

        txn.setInterruptOp(true);
        getReplCoord()->interrupt(opID);
        ReplicationCoordinator::StatusAndDuration statusAndDur = awaiter.getResult();
        ASSERT_EQUALS(ErrorCodes::Interrupted, statusAndDur.status);
        awaiter.reset();
    }

    class StepDownTest : public ReplCoordTest {

        virtual void setUp() {
            ReplCoordTest::setUp();
            init("mySet/test1:1234,test2:1234,test3:1234");
            assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 1 <<
                         "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                                 BSON("_id" << 1 << "host" << "test2:1234") <<
                                                 BSON("_id" << 2 << "host" << "test3:1234"))),
                    HostAndPort("test1", 1234));
            rid1 = getReplCoord()->getMyRID();
            rid2 = OID::gen();
            rid3 = OID::gen();
            HandshakeArgs handshake2;
            handshake2.initialize(BSON("handshake" << rid2 <<
                                       "member" << 1 <<
                                       "config" << BSON("_id" << 1 << "host" << "test2:1234")));
            HandshakeArgs handshake3;
            handshake3.initialize(BSON("handshake" << rid3 <<
                                       "member" << 2 <<
                                       "config" << BSON("_id" << 2 << "host" << "test3:1234")));
            OperationContextNoop txn;
            ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));
            ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake3));

            // Set state to SECONDARY so that later if we set it to PRIMARY then step down it
            // goes back to SECONDARY and not STARTUP2.
            getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_SECONDARY);
        }

    protected:
        OID rid1;
        OID rid2;
        OID rid3;
    };

    TEST_F(StepDownTest, StepDownNotPrimary) {
        OperationContextNoop txn;
        OpTime optime1(1, 1);
        // All nodes are caught up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime1));

        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_SECONDARY);
        Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(0));
        ASSERT_EQUALS(ErrorCodes::NotMaster, status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());
    }

    TEST_F(StepDownTest, StepDownTimeoutAcquiringGlobalLock) {
        OperationContextNoop txn;
        OpTime optime1(1, 1);
        // All nodes are caught up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime1));

        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);
        getExternalState()->setCanAcquireGlobalSharedLock(false);
        Status status = getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000));
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().primary());
    }

    TEST_F(StepDownTest, StepDownNoWaiting) {
        OperationContextNoop txn;
        OpTime optime1(1, 1);
        // All nodes are caught up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime1));

        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);
        ASSERT_TRUE(getTopoCoord().getMemberState().primary());
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().primary());
        ASSERT_OK(getReplCoord()->stepDown(&txn, false, Milliseconds(0), Milliseconds(1000)));
        ASSERT_EQUALS(Date_t(getNet()->now().millis + 1000), getTopoCoord().getStepDownTime());
        ASSERT_TRUE(getTopoCoord().getMemberState().secondary());
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());
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

        StepDownRunner(ReplicationCoordinatorImpl* replCoord) :
            _replCoord(replCoord), _finished(false), _result(Status::OK()), _force(false),
            _waitTime(0), _stepDownTime(0) {}

        // may block
        Status getResult() {
            _thread->join();
            ASSERT(_finished);
            return _result;
        }

        void start(OperationContext* txn) {
            ASSERT(!_finished);
            _thread.reset(new boost::thread(stdx::bind(&StepDownRunner::_stepDown,
                                                       this,
                                                       txn)));
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
        boost::scoped_ptr<boost::thread> _thread;
        bool _force;
        Milliseconds _waitTime;
        Milliseconds _stepDownTime;
    };

    TEST_F(StepDownTest, StepDownNotCaughtUp) {
        OperationContextNoop txn;
        OpTime optime1(1, 1);
        OpTime optime2(1, 2);
        // No secondary is caught up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime1));

        // Try to stepDown but time out because no secondaries are caught up
        StepDownRunner runner(getReplCoord());
        runner.setForce(false);
        runner.setWaitTime(Milliseconds(0));
        runner.setStepDownTime(Milliseconds(1000));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);

        runner.start(&txn);
        Status status = runner.getResult();
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().primary());

        // Now use "force" to force it to step down even though no one is caught up
        runner.reset();
        getNet()->incrementNow(Milliseconds(1000));
        runner.setForce(true);
        runner.start(&txn);
        status = runner.getResult();
        ASSERT_OK(status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());

    }

    TEST_F(StepDownTest, StepDownCatchUp) {
        OperationContextNoop txn;
        OpTime optime1(1, 1);
        OpTime optime2(1, 2);
        // No secondary is caught up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime1));

        // stepDown where the secondary actually has to catch up before the stepDown can succeed
        StepDownRunner runner(getReplCoord());
        runner.setForce(false);
        runner.setWaitTime(Milliseconds(1000));
        runner.setStepDownTime(Milliseconds(1000));
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);
        runner.start(&txn);
        // Make a secondary actually catch up
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime2));
        ASSERT_OK(runner.getResult());
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());
    }

    TEST_F(ReplCoordTest, GetReplicationModeNone) {
        init();
        ASSERT_EQUALS(ReplicationCoordinator::modeNone, getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeMaster) {
        // modeMasterSlave if master set
        ReplSettings settings;
        settings.master = true;
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave,
                      getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeSlave) {
        // modeMasterSlave if the slave flag was set
        ReplSettings settings;
        settings.slave = SimpleSlave;
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeMasterSlave,
                      getReplCoord()->getReplicationMode());
    }

    TEST_F(ReplCoordTest, GetReplicationModeRepl) {
        // modeReplSet only once config isInitialized
        ReplSettings settings;
        settings.replSet = "mySet/node1:12345";
        init(settings);
        ASSERT_EQUALS(ReplicationCoordinator::modeNone,
                      getReplCoord()->getReplicationMode());
        assertStart(
                ReplicationCoordinator::modeReplSet,
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("host" << "node1:12345" << "_id" << 0 ))),
                HostAndPort("node1", 12345));
    }

    TEST_F(ReplCoordTest, TestPrepareReplSetUpdatePositionCommand) {
        OperationContextNoop txn;
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                             BSON("_id" << 1 << "host" << "test2:1234") <<
                                             BSON("_id" << 2 << "host" << "test3:1234"))),
                HostAndPort("test1", 1234));
        OID rid1 = getReplCoord()->getMyRID();
        OID rid2 = OID::gen();
        OID rid3 = OID::gen();
        HandshakeArgs handshake2;
        handshake2.initialize(BSON("handshake" << rid2 <<
                                   "member" << 1 <<
                                   "config" << BSON("_id" << 1 << "host" << "test2:1234")));
        HandshakeArgs handshake3;
        handshake3.initialize(BSON("handshake" << rid3 <<
                                   "member" << 2 <<
                                   "config" << BSON("_id" << 2 << "host" << "test3:1234")));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake3));
        OpTime optime1(1, 1);
        OpTime optime2(1, 2);
        OpTime optime3(2, 1);
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid1, optime1));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid2, optime2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, rid3, optime3));

        // Check that the proper BSON is generated for the replSetUpdatePositionCommand
        BSONObjBuilder cmdBuilder;
        getReplCoord()->prepareReplSetUpdatePositionCommand(&txn, &cmdBuilder);
        BSONObj cmd = cmdBuilder.done();

        ASSERT_EQUALS(2, cmd.nFields());
        ASSERT_EQUALS("replSetUpdatePosition", cmd.firstElement().fieldNameStringData());

        std::set<OID> rids;
        BSONForEach(entryElement, cmd["optimes"].Obj()) {
            BSONObj entry = entryElement.Obj();
            OID rid = entry["_id"].OID();
            rids.insert(rid);
            if (rid == rid1) {
                ASSERT_EQUALS(optime1, entry["optime"]._opTime());
            } else if (rid == rid2) {
                ASSERT_EQUALS(optime2, entry["optime"]._opTime());
            } else {
                ASSERT_EQUALS(rid3, rid);
                ASSERT_EQUALS(optime3, entry["optime"]._opTime());
            }
        }
        ASSERT_EQUALS(3U, rids.size()); // Make sure we saw all 3 nodes
    }

    TEST_F(ReplCoordTest, TestHandshakes) {
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                             BSON("_id" << 1 << "host" << "test2:1234") <<
                                             BSON("_id" << 2 << "host" << "test3:1234"))),
                HostAndPort("test2", 1234));
        // Test generating basic handshake with no chaining
        std::vector<BSONObj> handshakes;
        OperationContextNoop txn;
        getReplCoord()->prepareReplSetUpdatePositionCommandHandshakes(&txn, &handshakes);
        ASSERT_EQUALS(1U, handshakes.size());
        BSONObj handshakeCmd = handshakes[0];
        ASSERT_EQUALS(2, handshakeCmd.nFields());
        ASSERT_EQUALS("replSetUpdatePosition", handshakeCmd.firstElement().fieldNameStringData());
        BSONObj handshake = handshakeCmd["handshake"].Obj();
        ASSERT_EQUALS(getReplCoord()->getMyRID(), handshake["handshake"].OID());
        ASSERT_EQUALS(1, handshake["member"].Int());
        handshakes.clear();

        // Have other nodes handshake us and make sure we process it right.
        OID slave1RID = OID::gen();
        OID slave2RID = OID::gen();
        HandshakeArgs slave1Handshake;
        slave1Handshake.initialize(BSON("handshake" << slave1RID <<
                                        "member" << 0 <<
                                        "config" << BSON("_id" << 0 << "host" << "test1:1234")));
        HandshakeArgs slave2Handshake;
        slave2Handshake.initialize(BSON("handshake" << slave2RID <<
                                        "member" << 2 <<
                                        "config" << BSON("_id" << 2 << "host" << "test2:1234")));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, slave1Handshake));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, slave2Handshake));

        getReplCoord()->prepareReplSetUpdatePositionCommandHandshakes(&txn, &handshakes);
        ASSERT_EQUALS(3U, handshakes.size());
        std::set<OID> rids;
        for (std::vector<BSONObj>::iterator it = handshakes.begin(); it != handshakes.end(); ++it) {
            BSONObj handshakeCmd = *it;
            ASSERT_EQUALS(2, handshakeCmd.nFields());
            ASSERT_EQUALS("replSetUpdatePosition",
                          handshakeCmd.firstElement().fieldNameStringData());

            BSONObj handshake = handshakeCmd["handshake"].Obj();
            OID rid = handshake["handshake"].OID();
            rids.insert(rid);
            if (rid == getReplCoord()->getMyRID()) {
                ASSERT_EQUALS(1, handshake["member"].Int());
            } else if (rid == slave1RID) {
                ASSERT_EQUALS(0, handshake["member"].Int());
            } else {
                ASSERT_EQUALS(slave2RID, rid);
                ASSERT_EQUALS(2, handshake["member"].Int());
            }
        }
        ASSERT_EQUALS(3U, rids.size()); // Make sure we saw all 3 nodes
    }

    TEST_F(ReplCoordTest, SetMaintenanceMode) {
        init("mySet/test1:1234,test2:1234,test3:1234");
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test1:1234") <<
                                             BSON("_id" << 1 << "host" << "test2:1234") <<
                                             BSON("_id" << 2 << "host" << "test3:1234"))),
                HostAndPort("test2", 1234));
        OperationContextNoop txn;

        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_SECONDARY);

        // Can't unset maintenance mode if it was never set to begin with.
        Status status = getReplCoord()->setMaintenanceMode(&txn, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());

        // valid set
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().recovering());

        // If we go into rollback while in maintenance mode, our state changes to RS_ROLLBACK.
        getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());

        // When we go back to SECONDARY, we still observe RECOVERING because of maintenance mode.
        getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().recovering());

        // Can set multiple times
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));

        // Need to unset the number of times you set
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
        status = getReplCoord()->setMaintenanceMode(&txn, false);
        // fourth one fails b/c we only set three times
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        // Unsetting maintenance mode changes our state to secondary if maintenance mode was
        // the only thinking keeping us out of it.
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());

        // From rollback, entering and exiting maintenance mode doesn't change perceived
        // state.
        getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());

        // Rollback is sticky even if entered while in maintenance mode.
        getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().recovering());
        getReplCoord()->setFollowerMode(MemberState::RS_ROLLBACK);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().rollback());
        getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());

        // Can't modify maintenance mode when PRIMARY
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_PRIMARY);
        status = getReplCoord()->setMaintenanceMode(&txn, true);
        ASSERT_EQUALS(ErrorCodes::NotSecondary, status);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().primary());
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_SECONDARY);
        status = getReplCoord()->setMaintenanceMode(&txn, false);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, true));
        ASSERT_OK(getReplCoord()->setMaintenanceMode(&txn, false));
    }

    TEST_F(ReplCoordTest, GetHostsWrittenToReplSet) {
        HostAndPort myHost("node1:12345");
        HostAndPort client1Host("node2:12345");
        HostAndPort client2Host("node3:12345") ;
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << myHost.toString()) <<
                                             BSON("_id" << 1 << "host" << client1Host.toString()) <<
                                             BSON("_id" << 2 << "host" << client2Host.toString()))),
                HostAndPort("node1", 12345));
        OperationContextNoop txn;

        OID myRID = getReplCoord()->getMyRID();
        OID client1 = OID::gen();
        OID client2 = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        HandshakeArgs handshake1;
        ASSERT_OK(handshake1.initialize(BSON("handshake" << client1 << "member" << 1)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake1));
        HandshakeArgs handshake2;
        ASSERT_OK(handshake2.initialize(BSON("handshake" << client2 << "member" << 2)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake2));

        ASSERT_OK(getReplCoord()->setLastOptime(&txn, myRID, time2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client1, time1));

        std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
        ASSERT_EQUALS(1U, caughtUpHosts.size());
        ASSERT_EQUALS(myHost, caughtUpHosts[0]);

        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client2, time2));
        caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
        ASSERT_EQUALS(2U, caughtUpHosts.size());
        if (myHost == caughtUpHosts[0]) {
            ASSERT_EQUALS(client2Host, caughtUpHosts[1]);
        }
        else {
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

        OID myRID = getReplCoord()->getMyRID();
        OID client = OID::gen();
        OpTime time1(1, 1);
        OpTime time2(1, 2);

        getExternalState()->setClientHostAndPort(clientHost);
        HandshakeArgs handshake;
        ASSERT_OK(handshake.initialize(BSON("handshake" << client)));
        ASSERT_OK(getReplCoord()->processHandshake(&txn, handshake));

        ASSERT_OK(getReplCoord()->setLastOptime(&txn, myRID, time2));
        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client, time1));

        std::vector<HostAndPort> caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
        ASSERT_EQUALS(0U, caughtUpHosts.size()); // self doesn't get included in master-slave

        ASSERT_OK(getReplCoord()->setLastOptime(&txn, client, time2));
        caughtUpHosts = getReplCoord()->getHostsWrittenTo(time2);
        ASSERT_EQUALS(1U, caughtUpHosts.size());
        ASSERT_EQUALS(clientHost, caughtUpHosts[0]);
    }

    TEST_F(ReplCoordTest, GetOtherNodesInReplSetNoConfig) {
        start();
        ASSERT_EQUALS(0U, getReplCoord()->getOtherNodesInReplSet().size());
    }

    TEST_F(ReplCoordTest, GetOtherNodesInReplSet) {
        assertStartSuccess(
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "h1") <<
                                             BSON("_id" << 1 << "host" << "h2") <<
                                             BSON("_id" << 2 <<
                                                  "host" << "h3" <<
                                                  "priority" << 0 <<
                                                  "hidden" << true))),
                HostAndPort("h1"));

        std::vector<HostAndPort> otherNodes = getReplCoord()->getOtherNodesInReplSet();
        ASSERT_EQUALS(2U, otherNodes.size());
        if (otherNodes[0] == HostAndPort("h2")) {
            ASSERT_EQUALS(HostAndPort("h3"), otherNodes[1]);
        }
        else {
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
                BSON("_id" << "mySet" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << h1.toString()) <<
                                             BSON("_id" << 1 << "host" << h2.toString()) <<
                                             BSON("_id" << 2 <<
                                                  "host" << h3.toString() <<
                                                  "arbiterOnly" << true) <<
                                             BSON("_id" << 3 <<
                                                  "host" << h4.toString() <<
                                                  "priority" << 0 <<
                                                  "tags" << BSON("key1" << "value1" <<
                                                                 "key2" << "value2")))),
                h4);
        getReplCoord()->_setCurrentMemberState_forTest(MemberState::RS_SECONDARY);
        ASSERT_TRUE(getReplCoord()->getCurrentMemberState().secondary());

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
        ASSERT_EQUALS(0, response.getSlaveDelay().total_seconds());
        ASSERT_EQUALS(h4, response.getMe());

        std::vector<HostAndPort> hosts = response.getHosts();
        ASSERT_EQUALS(2U, hosts.size());
        if (hosts[0] == h1) {
            ASSERT_EQUALS(h2, hosts[1]);
        }
        else {
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

    // TODO(schwerin): Unit test election id updating

}  // namespace
}  // namespace repl
}  // namespace mongo
