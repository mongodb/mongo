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
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_test_fixture.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    class ReplCoordElectTest : public ReplCoordTest {
    protected:
        void simulateEnoughHeartbeatsForElectability();
        void simulateFreshEnoughForElectability();
    };

    void ReplCoordElectTest::simulateEnoughHeartbeatsForElectability()  {
        ReplicationCoordinatorImpl* replCoord = getReplCoord();
        ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            ReplSetHeartbeatArgs hbArgs;
            if (hbArgs.initialize(request.cmdObj).isOK()) {
                ReplSetHeartbeatResponse hbResp;
                hbResp.setSetName(rsConfig.getReplSetName());
                hbResp.setState(MemberState::RS_SECONDARY);
                hbResp.setVersion(rsConfig.getConfigVersion());
                BSONObjBuilder respObj;
                respObj << "ok" << 1;
                hbResp.addToBSON(&respObj);
                net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
            }
            else {
                error() << "Black holing unexpected request to " << request.target << ": " <<
                    request.cmdObj;
                net->blackHole(noi);
            }
            net->runReadyNetworkOperations();
        }
        net->exitNetwork();
    }

    void ReplCoordElectTest::simulateFreshEnoughForElectability() {
        ReplicationCoordinatorImpl* replCoord = getReplCoord();
        ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            if (request.cmdObj.firstElement().fieldNameStringData() == "replSetFresh") {
                net->scheduleResponse(noi, net->now(), makeResponseStatus(
                                              BSON("ok" << 1 <<
                                                   "fresher" << false <<
                                                   "opTime" << Date_t(OpTime(0, 0).asDate()) <<
                                                   "veto" << false)));
            }
            else {
                error() << "Black holing unexpected request to " << request.target << ": " <<
                    request.cmdObj;
                net->blackHole(noi);
            }
            net->runReadyNetworkOperations();
        }
        net->exitNetwork();
    }

    TEST_F(ReplCoordElectTest, ElectTooSoon) {
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
        // Election fails because we haven't set a lastOpTimeApplied value yet, via a heartbeat.
        startCapturingLogMessages();
        assertStartSuccess(
            BSON("_id" << "mySet" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345") <<
                                         BSON("_id" << 2 << "host" << "node2:12345"))),
            HostAndPort("node1", 12345));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        simulateEnoughHeartbeatsForElectability();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("do not yet have a complete set of data"));
    }

    TEST_F(ReplCoordElectTest, Elect1NodeSuccess) {
        startCapturingLogMessages();
        assertStartSuccess(
            BSON("_id" << "mySet" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
            HostAndPort("node1", 12345));

        OperationContextNoop txn;
        getReplCoord()->setMyLastOptime(&txn, OpTime(1, 1));

        ASSERT(getReplCoord()->getCurrentMemberState().primary()) <<
            getReplCoord()->getCurrentMemberState().toString();
    }

    TEST_F(ReplCoordElectTest, ElectManyNodesSuccess) {
        BSONObj configObj = BSON("_id" << "mySet" <<
                                 "version" << 1 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345")
                                                      << BSON("_id" << 2 << "host" << "node2:12345")
                                                      << BSON("_id" << 3 << "host" << "node3:12345")
                                ));
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        OperationContextNoop txn;
        getReplCoord()->setMyLastOptime(&txn, OpTime(1, 1));
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
        startCapturingLogMessages();
        simulateSuccessfulElection();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
    }

    TEST_F(ReplCoordElectTest, ElectNotEnoughVotes) {
        // one responds with -10000 votes, and one doesn't respond, and we are not elected
        startCapturingLogMessages();
        BSONObj configObj = BSON("_id" << "mySet" <<
                                 "version" << 1 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345")
                                                      << BSON("_id" << 2 << "host" << "node2:12345")
                                                      << BSON("_id" << 3 << "host" << "node3:12345")
                                ));
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplicaSetConfig config = assertMakeRSConfig(configObj);

        OperationContextNoop txn;
        OID selfRID = getReplCoord()->getMyRID();
        OpTime time1(1, 1);
        getReplCoord()->setLastOptime(&txn, selfRID, time1);
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        simulateEnoughHeartbeatsForElectability();
        simulateFreshEnoughForElectability();
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        while (net->hasReadyRequests()) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            if (request.target != HostAndPort("node2", 12345)) {
                net->blackHole(noi);
            }
            else if (request.cmdObj.firstElement().fieldNameStringData() != "replSetElect") {
                net->blackHole(noi);
            }
            else {
                net->scheduleResponse(
                        noi,
                        net->now(),
                        makeResponseStatus(BSON("ok" << 1 <<
                                                "vote" << -10000 <<
                                                "round" << OID())));
            }
            net->runReadyNetworkOperations();
        }
        net->exitNetwork();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1,
                countLogLinesContaining("replSet couldn't elect self, only received -9999 votes"));
    }

    TEST_F(ReplCoordElectTest, ElectWrongTypeForVote) {
        // one responds with -10000 votes, and one doesn't respond, and we are not elected
        startCapturingLogMessages();
        BSONObj configObj = BSON("_id" << "mySet" <<
                                 "version" << 1 <<
                                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345")
                                                      << BSON("_id" << 2 << "host" << "node2:12345")
                                                      << BSON("_id" << 3 << "host" << "node3:12345")
                                ));
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        ReplicaSetConfig config = assertMakeRSConfig(configObj);

        OperationContextNoop txn;
        OID selfRID = getReplCoord()->getMyRID();
        OpTime time1(1, 1);
        getReplCoord()->setLastOptime(&txn, selfRID, time1);
        ASSERT(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));

        simulateEnoughHeartbeatsForElectability();
        simulateFreshEnoughForElectability();
        NetworkInterfaceMock* net = getNet();
        net->enterNetwork();
        while (net->hasReadyRequests()) {
            const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
            const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
            log() << request.target.toString() << " processing " << request.cmdObj;
            if (request.target != HostAndPort("node2", 12345)) {
                net->blackHole(noi);
            }
            else if (request.cmdObj.firstElement().fieldNameStringData() != "replSetElect") {
                net->blackHole(noi);
            }
            else {
                net->scheduleResponse(
                        noi,
                        net->now(),
                        makeResponseStatus(BSON("ok" << 1 <<
                                                "vote" << "yea" <<
                                                "round" << OID())));
            }
            net->runReadyNetworkOperations();
        }
        net->exitNetwork();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1,
                countLogLinesContaining("wrong type for vote argument in replSetElect command"));
    }

// TODO(dannenberg) reenable this test once we can ensure message ordering
//                  This test relies on the first message arriving prior to the second
//     TEST_F(ReplCoordElectTest, ElectWrongTypeForVoteButStillElected) {
//         // one responds with String for votes
//         startCapturingLogMessages();
//         BSONObj configObj = BSON("_id" << "mySet" <<
//                                  "version" << 1 <<
//                                  "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345")
//                                                       << BSON("_id" << 2 << "host" << "node2:12345")
//                                                       << BSON("_id" << 3 << "host" << "node3:12345")
//                                 ));
//         assertStartSuccess(configObj, HostAndPort("node1", 12345));
//         ReplicaSetConfig config = assertMakeRSConfig(configObj);

//         OperationContextNoop txn;
//         OID selfRID = getReplCoord()->getMyRID();
//         OpTime time1(1, 1);
//         getReplCoord()->setLastOptime(&txn, selfRID, time1);

//         const BSONObj electRequest = makeElectRequest(config, 0);

//         getNet()->addResponse(RemoteCommandRequest(HostAndPort("node2:12345"),
//                                                    "admin",
//                                                    electRequest),
//                               StatusWith<BSONObj>(BSON("ok" << 1 <<
//                                                        "vote" << 1 <<
//                                                        "round" << 380857196671097771ll)));
//         getNet()->addResponse(RemoteCommandRequest(HostAndPort("node3:12345"),
//                                                    "admin",
//                                                    electRequest),
//                               StatusWith<BSONObj>(BSON("ok" << 1 <<
//                                                        "vote" << "yea" <<
//                                                        "round" << 380857196671097771ll)));

//         getReplCoord()->testElection();
//         stopCapturingLogMessages();
//         ASSERT_EQUALS(1,
//                 countLogLinesContaining(
//                     "wrong type for vote argument in replSetElect command: String"));
//         ASSERT_EQUALS(1,
//                 countLogLinesContaining("replSet election succeeded, assuming primary role"));
//     }

}
}
}
