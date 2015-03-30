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
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/replication_coordinator.h" // ReplSetReconfigArgs
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationCoordinator::ReplSetReconfigArgs ReplSetReconfigArgs;
    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    TEST_F(ReplCoordTest, ReconfigBeforeInitialized) {
        // start up but do not initiate
        OperationContextNoop txn;
        init();
        start();
        BSONObjBuilder result;
        ReplSetReconfigArgs args;

        ASSERT_EQUALS(ErrorCodes::NotYetInitialized,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, ReconfigWhileNotPrimary) {
        // start up, become secondary, receive reconfig
        OperationContextNoop txn;
        init();
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));

        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        ASSERT_EQUALS(ErrorCodes::NotMaster,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, ReconfigWithUninitializableConfig) {
        // start up, become primary, receive uninitializable config
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << 2 <<
                                 "invalidlyNamedField" << 3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345" <<
                                                              "arbiterOnly" << true) <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345" <<
                                                              "arbiterOnly" << true)));
        // ErrorCodes::BadValue should be propagated from ReplicaSetConfig::initialize()
        ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, ReconfigWithWrongReplSetName) {
        // start up, become primary, receive config with incorrect replset name
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "notMySet" <<
                                 "version" << 3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345") <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345")));

        ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());
    }

    TEST_F(ReplCoordTest, ReconfigValidateFails) {
        // start up, become primary, validate fails
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << -3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345") <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345")));

        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());
    }

    void doReplSetInitiate(ReplicationCoordinatorImpl* replCoord, Status* status) {
        OperationContextNoop txn;
        BSONObjBuilder garbage;
        *status = replCoord->processReplSetInitiate(
                &txn,
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "node1:12345") <<
                             BSON("_id" << 2 << "host" << "node2:12345"))),
                &garbage);
    }

    void doReplSetReconfig(ReplicationCoordinatorImpl* replCoord, Status* status) {
        OperationContextNoop txn;
        BSONObjBuilder garbage;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << 3 <<
                                 "members" << BSON_ARRAY(
                                        BSON("_id" << 1 << "host" << "node1:12345") <<
                                        BSON("_id" << 2 << "host" << "node2:12345" <<
                                             "priority" << 3)));
        *status = replCoord->processReplSetReconfig(&txn, args, &garbage);
    }

    TEST_F(ReplCoordTest, ReconfigQuorumCheckFails) {
        // start up, become primary, fail during quorum check due to a heartbeat
        // containing a higher config version
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        Status status(ErrorCodes::InternalError, "Not Set");
        boost::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

        NetworkInterfaceMock* net = getNet();
        getNet()->enterNetwork();
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
        repl::ReplSetHeartbeatArgs hbArgs;
        ASSERT_OK(hbArgs.initialize(request.cmdObj));
        repl::ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setVersion(5);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj);
        net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
        net->runReadyNetworkOperations();
        getNet()->exitNetwork();
        reconfigThread.join();
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    }

    TEST_F(ReplCoordTest, ReconfigStoreLocalConfigDocumentFails) {
        // start up, become primary, saving the config fails
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        Status status(ErrorCodes::InternalError, "Not Set");
        getExternalState()->setStoreLocalConfigDocumentStatus(Status(ErrorCodes::OutOfDiskSpace, 
                                                                     "The test set this"));
        boost::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

        NetworkInterfaceMock* net = getNet();
        getNet()->enterNetwork();
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
        repl::ReplSetHeartbeatArgs hbArgs;
        ASSERT_OK(hbArgs.initialize(request.cmdObj));
        repl::ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setVersion(2);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj);
        net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
        net->runReadyNetworkOperations();
        getNet()->exitNetwork();
        reconfigThread.join();
        ASSERT_EQUALS(ErrorCodes::OutOfDiskSpace, status);
    }

    TEST_F(ReplCoordTest, ReconfigWhileReconfiggingFails) {
        // start up, become primary, reconfig, then before that reconfig concludes, reconfig again
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        Status status(ErrorCodes::InternalError, "Not Set");
        // first reconfig
        boost::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));
        getNet()->enterNetwork();
        getNet()->blackHole(getNet()->getNextReadyRequest());
        getNet()->exitNetwork();

        // second reconfig
        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << 3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345") <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345")));

        ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());

        shutdown();
        reconfigThread.join();
    }

    TEST_F(ReplCoordTest, ReconfigWhileInitializingFails) {
        // start up, initiate, then before that initiate concludes, reconfig
        OperationContextNoop txn;
        init();
        start(HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));

        // initiate
        Status status(ErrorCodes::InternalError, "Not Set");
        boost::thread initateThread(stdx::bind(doReplSetInitiate, getReplCoord(), &status));
        getNet()->enterNetwork();
        getNet()->blackHole(getNet()->getNextReadyRequest());
        getNet()->exitNetwork();

        // reconfig
        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << 3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345") <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345")));

        ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));
        ASSERT_TRUE(result.obj().isEmpty());

        shutdown();
        initateThread.join();
    }

    TEST_F(ReplCoordTest, ReconfigSuccessful) {
        // start up, become primary, reconfig successfully
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345"))),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));
        simulateSuccessfulElection();

        Status status(ErrorCodes::InternalError, "Not Set");
        boost::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

        NetworkInterfaceMock* net = getNet();
        getNet()->enterNetwork();
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
        repl::ReplSetHeartbeatArgs hbArgs;
        ASSERT_OK(hbArgs.initialize(request.cmdObj));
        repl::ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_SECONDARY);
        hbResp.setVersion(2);
        BSONObjBuilder respObj;
        respObj << "ok" << 1;
        hbResp.addToBSON(&respObj);
        net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
        net->runReadyNetworkOperations();
        getNet()->exitNetwork();
        reconfigThread.join();
        ASSERT_OK(status);
    }

    TEST_F(ReplCoordTest, ReconfigDuringHBReconfigFails) {
        // start up, become primary, receive reconfig via heartbeat, then a second one
        // from reconfig
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                          "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100,0));
        simulateSuccessfulElection();
        ASSERT_TRUE(getReplCoord()->getMemberState().primary());

        // set hbreconfig to hang while in progress
        getExternalState()->setStoreLocalConfigDocumentToHang(true);

        // hb reconfig
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        ReplSetHeartbeatResponse hbResp2;
        ReplicaSetConfig config;
        config.initialize(BSON("_id" << "mySet" <<
                               "version" << 3 <<
                               "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                            "host" << "node1:12345") <<
                                                       BSON("_id" << 2 <<
                                                            "host" << "node2:12345"))));
        hbResp2.setConfig(config);
        hbResp2.setVersion(3);
        hbResp2.setSetName("mySet");
        hbResp2.setState(MemberState::RS_SECONDARY);
        BSONObjBuilder respObj2;
        respObj2 << "ok" << 1;
        hbResp2.addToBSON(&respObj2);
        net->runUntil(net->now() + 10*1000); // run until we've sent a heartbeat request
        const NetworkInterfaceMock::NetworkOperationIterator noi2 = net->getNextReadyRequest();
        net->scheduleResponse(noi2, net->now(), makeResponseStatus(respObj2.obj()));
        net->runReadyNetworkOperations();
        getNet()->exitNetwork();

        // reconfig
        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = config.toBSON();
        ASSERT_EQUALS(ErrorCodes::ConfigurationInProgress,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));

        getExternalState()->setStoreLocalConfigDocumentToHang(false);
    }

    TEST_F(ReplCoordTest, HBReconfigDuringReconfigFails) {
        // start up, become primary, reconfig, while reconfigging receive reconfig via heartbeat
        OperationContextNoop txn;
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100,0));
        simulateSuccessfulElection();
        ASSERT_TRUE(getReplCoord()->getMemberState().primary());
 
        // start reconfigThread
        Status status(ErrorCodes::InternalError, "Not Set");
        boost::thread reconfigThread(stdx::bind(doReplSetReconfig, getReplCoord(), &status));

        // wait for reconfigThread to create network requests to ensure the replication coordinator
        // is in state kConfigReconfiguring
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        net->blackHole(net->getNextReadyRequest());

        // schedule hb reconfig
        net->runUntil(net->now() + 10*1000); // run until we've sent a heartbeat request
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        ReplSetHeartbeatResponse hbResp;
        ReplicaSetConfig config;
        config.initialize(BSON("_id" << "mySet" <<
                               "version" << 4 <<
                               "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                            "host" << "node1:12345") <<
                                                       BSON("_id" << 2 <<
                                                            "host" << "node2:12345"))));
        hbResp.setConfig(config);
        hbResp.setVersion(4);
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_SECONDARY);
        BSONObjBuilder respObj2;
        respObj2 << "ok" << 1;
        hbResp.addToBSON(&respObj2);
        net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj2.obj()));

        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
        startCapturingLogMessages();
        // execute hb reconfig, which should fail with a log message; confirmed at end of test
        net->runReadyNetworkOperations();
        // respond to reconfig's quorum check so that we can join that thread and exit cleanly
        net->exitNetwork();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1,
                countLogLinesContaining("because already in the midst of a configuration process"));
        shutdown();
        reconfigThread.join();
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Log());
    }

    TEST_F(ReplCoordTest, ForceReconfigWhileNotPrimarySuccessful) {
        // start up, become a secondary, receive a forced reconfig
        OperationContextNoop txn;
        init();
        assertStartSuccess(
                    BSON("_id" << "mySet" <<
                         "version" << 2 <<
                         "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                                 BSON("_id" << 2 << "host" << "node2:12345") )),
                    HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        getReplCoord()->setMyLastOptime(OpTime(100, 0));

        // fail before forced
        BSONObjBuilder result;
        ReplSetReconfigArgs args;
        args.force = false;
        args.newConfigObj = BSON("_id" << "mySet" <<
                                 "version" << 3 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 <<
                                                              "host" << "node1:12345") <<
                                                         BSON("_id" << 2 <<
                                                              "host" << "node2:12345")));
        ASSERT_EQUALS(ErrorCodes::NotMaster,
                      getReplCoord()->processReplSetReconfig(&txn, args, &result));

        // forced should succeed
        args.force = true;
        ASSERT_OK(getReplCoord()->processReplSetReconfig(&txn, args, &result));
        getReplCoord()->processReplSetGetConfig(&result);

        // ensure forced reconfig results in a random larger version
        ASSERT_GREATER_THAN(result.obj()["config"].Obj()["version"].numberInt(), 3);
    }

} // anonymous namespace
} // namespace repl
} // namespace mongo
