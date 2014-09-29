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
#include "mongo/db/repl/elect_cmd_runner.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    class ElectCmdRunnerTest : public mongo::unittest::Test {
    public:
        void startTest(ElectCmdRunner* electCmdRunner,
                       const ReplicaSetConfig& currentConfig,
                       int selfIndex,
                       const std::vector<HostAndPort>& hosts);

        void waitForTest();

        void electCmdRunnerRunner(const ReplicationExecutor::CallbackData& data,
                                  ElectCmdRunner* electCmdRunner,
                                  StatusWith<ReplicationExecutor::EventHandle>* evh,
                                  const ReplicaSetConfig& currentConfig,
                                  int selfIndex,
                                  const std::vector<HostAndPort>& hosts);

        NetworkInterfaceMock* _net;
        boost::scoped_ptr<ReplicationExecutor> _executor;
        boost::scoped_ptr<boost::thread> _executorThread;

    private:
        void setUp();
        void tearDown();

        ReplicationExecutor::EventHandle _allDoneEvent;
    };

    void ElectCmdRunnerTest::setUp() {
        _net = new NetworkInterfaceMock;
        _executor.reset(new ReplicationExecutor(_net, 1 /* prng seed */));
        _executorThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                           _executor.get())));
    }

    void ElectCmdRunnerTest::tearDown() {
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
                    "round" << 380865962699346850ll);
    }

    // This is necessary because the run method must be scheduled in the Replication Executor
    // for correct concurrency operation.
    void ElectCmdRunnerTest::electCmdRunnerRunner(
            const ReplicationExecutor::CallbackData& data,
            ElectCmdRunner* electCmdRunner,
            StatusWith<ReplicationExecutor::EventHandle>* evh,
            const ReplicaSetConfig& currentConfig,
            int selfIndex,
            const std::vector<HostAndPort>& hosts) {

        invariant(data.status.isOK());
        *evh = electCmdRunner->start(
                data.executor,
                currentConfig,
                selfIndex,
                hosts);
    }

    void ElectCmdRunnerTest::startTest(ElectCmdRunner* electCmdRunner,
                                       const ReplicaSetConfig& currentConfig,
                                       int selfIndex,
                                       const std::vector<HostAndPort>& hosts) {

        StatusWith<ReplicationExecutor::EventHandle> evh(ErrorCodes::InternalError, "Not set");
        StatusWith<ReplicationExecutor::CallbackHandle> cbh =
            _executor->scheduleWork(
                stdx::bind(&ElectCmdRunnerTest::electCmdRunnerRunner,
                           this,
                           stdx::placeholders::_1,
                           electCmdRunner,
                           &evh,
                           currentConfig,
                           selfIndex,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());
        ASSERT_OK(evh.getStatus());
        _allDoneEvent = evh.getValue();
    }

    void ElectCmdRunnerTest::waitForTest() {
        _executor->waitForEvent(_allDoneEvent);
    }

    TEST_F(ElectCmdRunnerTest, OneNode) {
        // Only one node in the config.
        const ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h1"))));

        std::vector<HostAndPort> hosts;
        ElectCmdRunner electCmdRunner;
        startTest(&electCmdRunner, config, 0, hosts);
        waitForTest();
        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);
    }

    TEST_F(ElectCmdRunnerTest, TwoNodes) {
        // Two nodes, we are node h1.
        const ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h0") <<
                     BSON("_id" << 2 << "host" << "h1"))));

        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        const BSONObj electRequest = makeElectRequest(config, 0);

        ElectCmdRunner electCmdRunner;
        startTest(&electCmdRunner, config, 0, hosts);
        const Date_t startDate = _net->now();
        _net->enterNetwork();
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(electRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(noi,
                               startDate + 10,
                               ResponseStatus(ReplicationExecutor::RemoteCommandResponse(
                                                      BSON("ok" << 1 <<
                                                           "vote" << 1 <<
                                                           "round" << 380865962699346850ll),
                                                      Milliseconds(8))));
        _net->runUntil(startDate + 10);
        _net->exitNetwork();
        ASSERT_EQUALS(startDate + 10, _net->now());
        waitForTest();
        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 2);
    }

    TEST_F(ElectCmdRunnerTest, ShuttingDown) {
        // Two nodes, we are node h1.  Shutdown happens while we're scheduling remote commands.
        ReplicaSetConfig config = assertMakeRSConfig(
            BSON("_id" << "rs0" <<
                 "version" << 1 <<
                 "members" << BSON_ARRAY(
                     BSON("_id" << 1 << "host" << "h0") <<
                     BSON("_id" << 2 << "host" << "h1"))));

        std::vector<HostAndPort> hosts;
        hosts.push_back(config.getMemberAt(1).getHostAndPort());

        ElectCmdRunner electCmdRunner;
        StatusWith<ReplicationExecutor::EventHandle> evh(ErrorCodes::InternalError, "Not set");
        StatusWith<ReplicationExecutor::CallbackHandle> cbh =
            _executor->scheduleWork(
                stdx::bind(&ElectCmdRunnerTest::electCmdRunnerRunner,
                           this,
                           stdx::placeholders::_1,
                           &electCmdRunner,
                           &evh,
                           config,
                           0,
                           hosts));
        ASSERT_OK(cbh.getStatus());
        _executor->wait(cbh.getValue());
        ASSERT_OK(evh.getStatus());
        _executor->shutdown();
        _executor->waitForEvent(evh.getValue());
        ASSERT_EQUALS(electCmdRunner.getReceivedVotes(), 1);
    }

    class ElectScatterGatherTest : public mongo::unittest::Test {
    public:
        virtual void start(const BSONObj& configObj) {
            int selfConfigIndex = 0;

            ReplicaSetConfig config;
            config.initialize(configObj);

            std::vector<HostAndPort> hosts;
            for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin();
                    mem != config.membersEnd();
                    ++mem) {
                hosts.push_back(mem->getHostAndPort());
            }

            _checker.reset(new ElectCmdRunner::Algorithm(config,
                                                         selfConfigIndex,
                                                         hosts,
                                                         1954878951734LL));
        }

        virtual void tearDown() {
            _checker.reset(NULL);
        }

    protected:
        bool hasReceivedSufficientResponses() {
            return _checker->hasReceivedSufficientResponses();
        }

        int getReceivedVotes() {
            return _checker->getReceivedVotes();
        }

        void processResponse(const RemoteCommandRequest& request, const ResponseStatus& response) {
            _checker->processResponse(request, response);
        }

        RemoteCommandRequest requestFrom(std::string hostname) {
            return RemoteCommandRequest(HostAndPort(hostname),
                                        "", // the non-hostname fields do not matter for Elect
                                        BSONObj(),
                                        Milliseconds(0));
        }

        ResponseStatus badResponseStatus() {
            return ResponseStatus(ErrorCodes::NodeNotFound, "not on my watch");
        }

        ResponseStatus wrongTypeForVoteField() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("vote" << std::string("yea")),
                                                                 Milliseconds(10)));
        }

        ResponseStatus voteYea() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("vote" << 1),
                                                                 Milliseconds(10)));
        }

        ResponseStatus voteNay() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("vote" << -10000),
                                                                 Milliseconds(10)));
        }

        ResponseStatus abstainFromVoting() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("vote" << 0),
                                                                 Milliseconds(10)));
        }

        BSONObj basicThreeNodeConfig() {
            return BSON("_id" << "rs0" <<
                        "version" << 1 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0") <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2")));
        }

    private:
        scoped_ptr<ElectCmdRunner::Algorithm> _checker;
    };

    TEST_F(ElectScatterGatherTest, NodeRespondsWithBadVoteType) {
        start(basicThreeNodeConfig());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host2"), wrongTypeForVoteField());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, getReceivedVotes()); // 1 because we have 1 vote and voted for ourself
    }

    TEST_F(ElectScatterGatherTest, NodeRespondsWithBadStatus) {
        start(basicThreeNodeConfig());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host2"), badResponseStatus());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host3"), abstainFromVoting());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, getReceivedVotes()); // 1 because we have 1 vote and voted for ourself
    }

    TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithYea) {
        start(basicThreeNodeConfig());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host2"), voteYea());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(2, getReceivedVotes());
    }

    TEST_F(ElectScatterGatherTest, FirstNodeRespondsWithNaySecondWithYea) {
        start(basicThreeNodeConfig());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host2"), voteNay());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host3"), voteYea());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(-9998, getReceivedVotes());
    }

    TEST_F(ElectScatterGatherTest, BothNodesAbstainFromVoting) {
        start(basicThreeNodeConfig());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host2"), abstainFromVoting());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom("host3"), abstainFromVoting());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, getReceivedVotes());
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
