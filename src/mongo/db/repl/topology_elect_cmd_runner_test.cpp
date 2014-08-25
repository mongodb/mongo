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

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/topology_elect_cmd_runner.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    class ElectCmdRunnerTest : public mongo::unittest::Test {
    public:
        ElectCmdRunnerTest();
        void electCmdRunnerRunner(const ReplicationExecutor::CallbackData& data,
                                  ElectCmdRunner* electCmdRunner,
                                  const ReplicationExecutor::EventHandle& evh,
                                  const ReplicaSetConfig& currentConfig,
                                  int selfIndex,
                                  const std::vector<MemberHeartbeatData>& hbData);
    protected:
        NetworkInterfaceMockWithMap* _net;
        boost::scoped_ptr<ReplicationExecutor> _executor;
        boost::scoped_ptr<boost::thread> _executorThread;
        Status _lastStatus;

    private:
        void setUp();
        void tearDown();
    };

    ElectCmdRunnerTest::ElectCmdRunnerTest() : _lastStatus(Status::OK()) {}

    void ElectCmdRunnerTest::setUp() {
        _net = new NetworkInterfaceMockWithMap;
        _executor.reset(new ReplicationExecutor(_net));
        _executorThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                           _executor.get())));
    }

    void ElectCmdRunnerTest::tearDown() {
        _net->unblockAll();
        _executor->shutdown();
        _executorThread->join();
    }

    ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBson) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(configBson));
        ASSERT_OK(config.validate());
        return config;
    }

    const BSONObj makeElectRequest(const ReplicaSetConfig& rsConfig, 
                                   int selfIndex) {
        const MemberConfig& myConfig = rsConfig.getMemberAt(selfIndex);
        return BSON("replSetElect" << 1 <<
                    "set" << rsConfig.getReplSetName() <<
                    "who" << myConfig.getHostAndPort().toString() <<
                    "whoid" << myConfig.getId() <<
                    "cfgver" << rsConfig.getConfigVersion() <<
                    "round" << 0);
    }

    // This is necessary because the run method must be scheduled in the Replication Executor
    // for correct concurrency operation.
    void ElectCmdRunnerTest::electCmdRunnerRunner(
        const ReplicationExecutor::CallbackData& data,
        ElectCmdRunner* electCmdRunner,
        const ReplicationExecutor::EventHandle& evh,
        const ReplicaSetConfig& currentConfig,
        int selfIndex,
        const std::vector<MemberHeartbeatData>& hbData) {
        invariant(data.status.isOK());
        _lastStatus = electCmdRunner->start(data.executor, 
                                            evh, 
                                            currentConfig, 
                                            selfIndex, 
                                            hbData);
    }

    TEST_F(ElectCmdRunnerTest, OneNode) {
        // Only one node in the config.
        ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<MemberHeartbeatData> hbData;
        MemberHeartbeatData h1Info(0);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        hbData.push_back(h1Info);

        ElectCmdRunner electCmdRunner;
        
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&ElectCmdRunnerTest::electCmdRunnerRunner,
                           this,
                           stdx::placeholders::_1,
                           &electCmdRunner,
                           evh.getValue(),
                           config,
                           0,
                           hbData));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());

        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);        
    }

    TEST_F(ElectCmdRunnerTest, TwoNodes) {
        // Two nodes, we are node h1.
        ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h0") <<
                     BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<MemberHeartbeatData> hbData;
        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        hbData.push_back(h0Info);
        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        hbData.push_back(h1Info);

        const BSONObj electRequest = makeElectRequest(config, 0);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               electRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "vote" << 1 <<
                                                   "round" << 0)));

        ElectCmdRunner electCmdRunner;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&ElectCmdRunnerTest::electCmdRunnerRunner,
                           this,
                           stdx::placeholders::_1,
                           &electCmdRunner,
                           evh.getValue(),
                           config,
                           0,
                           hbData));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());

        ASSERT_OK(_lastStatus);

        _executor->waitForEvent(evh.getValue());
        // TODO: this ought to pass once elect cmd is implemented.
        //ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 2);
        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);
        
    }


    TEST_F(ElectCmdRunnerTest, ShuttingDown) {
        // Two nodes, we are node h1.  Shutdown happens while we're scheduling remote commands.
        ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h0") <<
                     BSON("_id" << 2 << "host" << "h1"))));
        
        StatusWith<ReplicationExecutor::EventHandle> evh = _executor->makeEvent();
        ASSERT_OK(evh.getStatus());

        Date_t now(0);
        std::vector<MemberHeartbeatData> hbData;
        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        hbData.push_back(h0Info);
        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        hbData.push_back(h1Info);

        const BSONObj electRequest = makeElectRequest(config, 0);
        _net->addResponse(RemoteCommandRequest(HostAndPort("h1"),
                                               "admin",
                                               electRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1 <<
                                                   "vote" << 1 <<
                                                   "round" << 0)),
                          true /* isBlocked */);

        ElectCmdRunner electCmdRunner;
        StatusWith<ReplicationExecutor::CallbackHandle> cbh = 
            _executor->scheduleWork(
                stdx::bind(&ElectCmdRunnerTest::electCmdRunnerRunner,
                           this,
                           stdx::placeholders::_1,
                           &electCmdRunner,
                           evh.getValue(),
                           config,
                           0,
                           hbData));
        ASSERT_OK(cbh.getStatus());

        _executor->wait(cbh.getValue());
        ASSERT_OK(_lastStatus);

        _executor->shutdown();
        _net->unblockAll();
        _executor->waitForEvent(evh.getValue());

        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);

    }


}  // namespace
}  // namespace repl
}  // namespace mongo
