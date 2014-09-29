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

#include <map>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_test_fixture.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

    bool operator<(const ReplicationExecutor::RemoteCommandRequest& lhs,
                   const ReplicationExecutor::RemoteCommandRequest& rhs) {
        if (lhs.target < rhs.target)
            return true;
        if (rhs.target < lhs.target)
            return false;
        if (lhs.dbname < rhs.dbname)
            return true;
        if (rhs.dbname < lhs.dbname)
            return false;
        return lhs.cmdObj < rhs.cmdObj;
    }

namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBson) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(configBson));
        ASSERT_OK(config.validate());
        return config;
    }

    // Since the ReplicationExecutor has the same seed (1) in each test, the randomly generated
    // "round" number is always the below.
    static const long long kFirstRound = 380857196671097771LL;

    const BSONObj makeElectRequest(const ReplicaSetConfig& rsConfig,
                                   int selfIndex) {
        const MemberConfig& myConfig = rsConfig.getMemberAt(selfIndex);
        return BSON("replSetElect" << 1 <<
                    "set" << rsConfig.getReplSetName() <<
                    "who" << myConfig.getHostAndPort().toString() <<
                    "whoid" << myConfig.getId() <<
                    "cfgver" << rsConfig.getConfigVersion() <<
                    "round" << kFirstRound);
    }

    void doNothing() {}

    class ReplCoordElectTest : public ReplCoordTest {
    protected:
        void addResponse(const ReplicationExecutor::RemoteCommandRequest& request,
                         const StatusWith<BSONObj>& response) {
            if (response.isOK()) {
                addResponse(request, ResponseStatus(ReplicationExecutor::RemoteCommandResponse(
                                                            response.getValue(), Milliseconds(8))));
            }
            else {
                addResponse(request, ResponseStatus(response.getStatus()));
            }
        }

        void addResponse(const ReplicationExecutor::RemoteCommandRequest& request,
                         const ResponseStatus& response) {
            ASSERT_FALSE(_networkThread);
            _responses[request] = response;
        }

        void startNetworkThread() {
            ASSERT_FALSE(_networkThread);
            _inShutdown = false;
            _networkThread.reset(
                    new boost::thread(&ReplCoordElectTest::_serveNetworkRequests, this));
        }

        void stopNetworkThread() {
            ASSERT(_networkThread);
            boost::unique_lock<boost::mutex> lk(_mutex);
            _inShutdown = true;
            lk.unlock();
            getNet()->startCommand(
                    ReplicationExecutor::CallbackHandle(),
                    ReplicationExecutor::RemoteCommandRequest(),
                    stdx::bind(doNothing));
            _networkThread->join();
        }

    private:
        struct Response {
            Response() : value(ErrorCodes::InternalError, "Never initialized") {}
            Response(ResponseStatus s) : value(s) {}
            ResponseStatus value;
        };
        typedef std::map<ReplicationExecutor::RemoteCommandRequest, Response> ResponseMap;

        void tearDown() {
            ReplCoordTest::tearDown();
            if (_networkThread) {
                _networkThread->join();
            }
        }

        void _serveNetworkRequests() {
            getNet()->enterNetwork();
            while (!_isInShutdown()) {
                const NetworkInterfaceMock::NetworkOperationIterator noi =
                    getNet()->getNextReadyRequest();
                getNet()->scheduleResponse(
                        noi,
                        getNet()->now(),
                        mapFindWithDefault(
                                _responses,
                                noi->getRequest(),
                                Response(ResponseStatus(ErrorCodes::NoSuchKey, "No known response"))).value);
                getNet()->runReadyNetworkOperations();
            }
            getNet()->exitNetwork();
        }

        bool _isInShutdown() {
            boost::lock_guard<boost::mutex> lk(_mutex);
            return _inShutdown;
        }

        boost::mutex _mutex;
        bool _inShutdown;
        ResponseMap _responses;
        boost::scoped_ptr<boost::thread> _networkThread;
    };

    TEST_F(ReplCoordElectTest, ElectTooSoon) {
        // Election fails because we haven't set a lastOpTimeApplied value yet, via a heartbeat.
        startCapturingLogMessages();
        assertStartSuccess(
            BSON("_id" << "mySet" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "node1:12345"))),
            HostAndPort("node1", 12345));
        getReplCoord()->testElection();
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
        OID selfRID = getReplCoord()->getMyRID();
        OpTime time1(1, 1);
        getReplCoord()->setLastOptime(&txn, selfRID, time1);

        getReplCoord()->testElection();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1, countLogLinesContaining("election succeeded"));
    }

    TEST_F(ReplCoordElectTest, ElectManyNodesSuccess) {
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

        const BSONObj electRequest = makeElectRequest(config, 0);
        addResponse(RemoteCommandRequest(HostAndPort("node2:12345"),
                                         "admin",
                                         electRequest),
                    StatusWith<BSONObj>(BSON("ok" << 1 <<
                                             "vote" << 1 <<
                                             "round" << kFirstRound)));

        addResponse(RemoteCommandRequest(HostAndPort("node3:12345"),
                                         "admin",
                                         electRequest),
                    StatusWith<BSONObj>(BSON("ok" << 1 <<
                                             "vote" << 1 <<
                                             "round" << kFirstRound)));

        startNetworkThread();
        getReplCoord()->testElection();
        stopNetworkThread();
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

        const BSONObj electRequest = makeElectRequest(config, 0);
        addResponse(RemoteCommandRequest(HostAndPort("node2:12345"),
                                         "admin",
                                         electRequest),
                    StatusWith<BSONObj>(BSON("ok" << 1 <<
                                             "vote" << -10000 <<
                                             "round" << kFirstRound)));

        startNetworkThread();
        getReplCoord()->testElection();
        stopNetworkThread();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1,
                countLogLinesContaining("replSet couldn't elect self, only received -9999 votes"));
    }

    TEST_F(ReplCoordElectTest, ElectWrongTypeForVote) {
        // one responds with String for votes
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

        const BSONObj electRequest = makeElectRequest(config, 0);
        addResponse(RemoteCommandRequest(HostAndPort("node2:12345"),
                                         "admin",
                                         electRequest),
                    StatusWith<BSONObj>(BSON("ok" << 1 <<
                                             "vote" << "yea" <<
                                             "round" << kFirstRound)));

        startNetworkThread();
        getReplCoord()->testElection();
        stopNetworkThread();
        stopCapturingLogMessages();
        ASSERT_EQUALS(1,
                countLogLinesContaining(
                    "wrong type for vote argument in replSetElect command: String"));
        ASSERT_EQUALS(1,
                countLogLinesContaining("replSet couldn't elect self, only received 1 votes"));
    }

//     TODO(dannenberg) reenable this test once we can ensure message ordering
//                      This test relies on the first message arriving prior to the second
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
