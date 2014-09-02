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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    class ReplCoordElectTest : public mongo::unittest::Test {
    public:
        virtual void setUp() {
            _settings.replSet = "mySet/node1:12345,node2:54321";
        }

        virtual void tearDown() {
            _repl->shutdown();
        }

    protected:
        NetworkInterfaceMockWithMap* getNet() { return _net; }
        ReplicationCoordinatorImpl* getReplCoord() { return _repl.get(); }

        void init() {
            invariant(!_repl);

            // PRNG seed for tests.
            const int64_t seed = 0;
            _externalState = new ReplicationCoordinatorExternalStateMock;
            _net = new NetworkInterfaceMockWithMap;
            _repl.reset(new ReplicationCoordinatorImpl(_settings,
                                                       _externalState,
                                                       _net,
                                                       new TopologyCoordinatorImpl(Seconds(999)),
                                                       seed));
        }

        void assertReplStart(const BSONObj& configDoc, const HostAndPort& selfHost) {
            init();
            _externalState->setLocalConfigDocument(StatusWith<BSONObj>(configDoc));
            _externalState->addSelf(selfHost);
            OperationContextNoop txn;
            _repl->startReplication(&txn);
            _repl->waitForStartUpComplete();

            ASSERT_EQUALS(ReplicationCoordinator::modeReplSet, 
                          getReplCoord()->getReplicationMode());
        }

        int64_t countLogLinesContaining(const std::string& needle) {
            return std::count_if(getCapturedLogMessages().begin(),
                                 getCapturedLogMessages().end(),
                                 stdx::bind(stringContains,
                                            stdx::placeholders::_1,
                                            needle));
        }

    private:
        boost::scoped_ptr<ReplicationCoordinatorImpl> _repl;
        // Owned by ReplicationCoordinatorImpl
        ReplicationCoordinatorExternalStateMock* _externalState;
        // Owned by ReplicationCoordinatorImpl
        NetworkInterfaceMockWithMap* _net;
        ReplSettings _settings;
    };

    TEST_F(ReplCoordElectTest, ElectTooSoon) {
        // Election fails because we haven't set a lastOpTimeApplied value yet, via a heartbeat.
        startCapturingLogMessages();
        assertReplStart(
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
        assertReplStart(
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

/*
    TEST_F(ReplCoordElectTest, ElectManyNodesSuccess) {}
    TEST_F(ReplCoordElectTest, ShutdownExecutor) {}

    // Uses PRNG
    TEST_F(ReplCoordElectTest, ElectTiedWithAnother) {}
*/

}
}
}
