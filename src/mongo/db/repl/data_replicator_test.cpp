/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/data_replicator.h"
#include "mongo/db/repl/fetcher.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/util/fail_point_service.h"

#include "mongo/unittest/unittest.h"

namespace {
    using namespace mongo;
    using namespace mongo::repl;

    const HostAndPort target("localhost", -1);

    class DataReplicatorTest : public ReplicationExecutorTest {
    public:
        DataReplicatorTest() {}
        void setUp() override {
            ReplicationExecutorTest::setUp();
            reset();

            // PRNG seed for tests.
            const int64_t seed = 0;

            ReplicationExecutor* exec = &(getExecutor());
            _topo = new TopologyCoordinatorImpl(Seconds(0));
            _externalState = new ReplicationCoordinatorExternalStateMock;
            _repl.reset(new ReplicationCoordinatorImpl(_settings,
                                                       _externalState,
                                                       _topo,
                                                       exec,
                                                       seed));
            launchExecutorThread();
            _dr.reset(new DataReplicator(DataReplicatorOptions(), exec, _repl.get()));
        }
        void tearDown() override {
            ReplicationExecutorTest::tearDown();
            // Executor may still invoke callback before shutting down.
        }

        void reset() {
            // clear/reset state

        }
        void scheduleNetworkResponse(const BSONObj& obj) {
            NetworkInterfaceMock* net = getNet();
            ASSERT_TRUE(net->hasReadyRequests());
            Milliseconds millis(0);
            RemoteCommandResponse response(obj, millis);
            ReplicationExecutor::ResponseStatus responseStatus(response);
            net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
        }

        void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
            NetworkInterfaceMock* net = getNet();
            ASSERT_TRUE(net->hasReadyRequests());
            ReplicationExecutor::ResponseStatus responseStatus(code, reason);
            net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
        }

        void processNetworkResponse(const BSONObj& obj) {
            scheduleNetworkResponse(obj);
            finishProcessingNetworkResponse();
        }

        void processNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
            scheduleNetworkResponse(code, reason);
            finishProcessingNetworkResponse();
        }

        void finishProcessingNetworkResponse() {
            getNet()->runReadyNetworkOperations();
            ASSERT_FALSE(getNet()->hasReadyRequests());
        }

        DataReplicator& getDR() { return *_dr; }

    protected:
        std::unique_ptr<DataReplicator> _dr;

    private:
        boost::scoped_ptr<ReplicationCoordinatorImpl> _repl;
        // Owned by ReplicationCoordinatorImpl
        TopologyCoordinatorImpl* _topo;
        // Owned by ReplicationCoordinatorImpl
        NetworkInterfaceMock* _net;
        // Owned by ReplicationCoordinatorImpl
        ReplicationCoordinatorExternalStateMock* _externalState;
        ReplSettings _settings;

    };

    TEST_F(DataReplicatorTest, CreateDestroy) {

    }

    TEST_F(DataReplicatorTest, CannotInitialSyncAfterStart) {
        ASSERT_EQ(getDR().start().code(), ErrorCodes::OK);
        ASSERT_EQ(getDR().initialSync(), ErrorCodes::AlreadyInitialized);
    }

    TEST_F(DataReplicatorTest, InitialSyncFailpoint) {
        mongo::getGlobalFailPointRegistry()->
                 getFailPoint("failInitialSyncWithBadHost")->
                    setMode(FailPoint::alwaysOn);

        ASSERT_EQ(getDR().initialSync(), ErrorCodes::InitialSyncFailure);

        mongo::getGlobalFailPointRegistry()->
                 getFailPoint("failInitialSyncWithBadHost")->
                    setMode(FailPoint::off);
    }
}
