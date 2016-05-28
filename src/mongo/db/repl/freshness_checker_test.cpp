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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/freshness_checker.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using std::unique_ptr;

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class FreshnessCheckerTest : public mongo::unittest::Test {
protected:
    void startTest(const Timestamp& lastOpTimeApplied,
                   const ReplicaSetConfig& currentConfig,
                   int selfIndex,
                   const std::vector<HostAndPort>& hosts);
    void waitOnChecker();
    FreshnessChecker::ElectionAbortReason shouldAbortElection() const;

    int64_t countLogLinesContaining(const std::string& needle) {
        return std::count_if(getCapturedLogMessages().begin(),
                             getCapturedLogMessages().end(),
                             stdx::bind(stringContains, stdx::placeholders::_1, needle));
    }

    NetworkInterfaceMock* _net;
    std::unique_ptr<ReplicationExecutor> _executor;
    std::unique_ptr<stdx::thread> _executorThread;

private:
    void freshnessCheckerRunner(const ReplicationExecutor::CallbackArgs& data,
                                const Timestamp& lastOpTimeApplied,
                                const ReplicaSetConfig& currentConfig,
                                int selfIndex,
                                const std::vector<HostAndPort>& hosts);
    void setUp();
    void tearDown();

    std::unique_ptr<FreshnessChecker> _checker;
    ReplicationExecutor::EventHandle _checkerDoneEvent;
};

void FreshnessCheckerTest::setUp() {
    _net = new NetworkInterfaceMock;
    _executor = stdx::make_unique<ReplicationExecutor>(_net, 1 /* prng seed */);
    _executorThread.reset(new stdx::thread(stdx::bind(&ReplicationExecutor::run, _executor.get())));
    _checker.reset(new FreshnessChecker);
}

void FreshnessCheckerTest::tearDown() {
    _executor->shutdown();
    _executorThread->join();
}

void FreshnessCheckerTest::waitOnChecker() {
    _executor->waitForEvent(_checkerDoneEvent);
}

FreshnessChecker::ElectionAbortReason FreshnessCheckerTest::shouldAbortElection() const {
    return _checker->shouldAbortElection();
}

ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBson) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(configBson));
    ASSERT_OK(config.validate());
    return config;
}

const BSONObj makeFreshRequest(const ReplicaSetConfig& rsConfig,
                               Timestamp lastOpTimeApplied,
                               int selfIndex) {
    const MemberConfig& myConfig = rsConfig.getMemberAt(selfIndex);
    return BSON("replSetFresh" << 1 << "set" << rsConfig.getReplSetName() << "opTime"
                               << Date_t::fromMillisSinceEpoch(lastOpTimeApplied.asLL())
                               << "who"
                               << myConfig.getHostAndPort().toString()
                               << "cfgver"
                               << rsConfig.getConfigVersion()
                               << "id"
                               << myConfig.getId());
}

// This is necessary because the run method must be scheduled in the Replication Executor
// for correct concurrency operation.
void FreshnessCheckerTest::freshnessCheckerRunner(const ReplicationExecutor::CallbackArgs& data,
                                                  const Timestamp& lastOpTimeApplied,
                                                  const ReplicaSetConfig& currentConfig,
                                                  int selfIndex,
                                                  const std::vector<HostAndPort>& hosts) {
    invariant(data.status.isOK());
    ReplicationExecutor* executor = dynamic_cast<ReplicationExecutor*>(data.executor);
    ASSERT(executor);
    StatusWith<ReplicationExecutor::EventHandle> evh =
        _checker->start(executor, lastOpTimeApplied, currentConfig, selfIndex, hosts);
    _checkerDoneEvent = assertGet(evh);
}

void FreshnessCheckerTest::startTest(const Timestamp& lastOpTimeApplied,
                                     const ReplicaSetConfig& currentConfig,
                                     int selfIndex,
                                     const std::vector<HostAndPort>& hosts) {
    _executor->wait(
        assertGet(_executor->scheduleWork(stdx::bind(&FreshnessCheckerTest::freshnessCheckerRunner,
                                                     this,
                                                     stdx::placeholders::_1,
                                                     lastOpTimeApplied,
                                                     currentConfig,
                                                     selfIndex,
                                                     hosts))));
}

TEST_F(FreshnessCheckerTest, TwoNodes) {
    // Two nodes, we are node h1.  We are freshest, but we tie with h2.
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());
    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(0, 0), 0);

    startTest(Timestamp(0, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            ResponseStatus(RemoteCommandResponse(BSON("ok" << 1 << "id" << 2 << "set"
                                                           << "rs0"
                                                           << "who"
                                                           << "h1"
                                                           << "cfgver"
                                                           << 1
                                                           << "opTime"
                                                           << Date_t()),
                                                 BSONObj(),
                                                 Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FreshnessTie);
}

TEST_F(FreshnessCheckerTest, ShuttingDown) {
    // Two nodes, we are node h1.  Shutdown happens while we're scheduling remote commands.
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    startTest(Timestamp(0, 0), config, 0, hosts);
    _executor->shutdown();
    waitOnChecker();

    // This seems less than ideal, but if we are shutting down, the next phase of election
    // cannot proceed anyway.
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::None);
}

TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshest) {
    // other responds as fresher than us
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            ResponseStatus(RemoteCommandResponse(BSON("ok" << 1 << "id" << 2 << "set"
                                                           << "rs0"
                                                           << "who"
                                                           << "h1"
                                                           << "cfgver"
                                                           << 1
                                                           << "fresher"
                                                           << true
                                                           << "opTime"
                                                           << Date_t()),
                                                 BSONObj(),
                                                 Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();

    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
}

TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestOpTime) {
    // other responds with a later optime than ours
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(0, 0), 0);

    startTest(Timestamp(0, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            ResponseStatus(RemoteCommandResponse(
                BSON("ok" << 1 << "id" << 2 << "set"
                          << "rs0"
                          << "who"
                          << "h1"
                          << "cfgver"
                          << 1
                          << "opTime"
                          << Date_t::fromMillisSinceEpoch(Timestamp(10, 0).asLL())),
                BSONObj(),
                Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();

    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessCheckerTest, ElectWrongTypeInFreshnessResponse) {
    // other responds with "opTime" field of non-Date value, causing not freshest
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            ResponseStatus(RemoteCommandResponse(BSON("ok" << 1 << "id" << 2 << "set"
                                                           << "rs0"
                                                           << "who"
                                                           << "h1"
                                                           << "cfgver"
                                                           << 1
                                                           << "opTime"
                                                           << 3),
                                                 BSONObj(),
                                                 Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();

    stopCapturingLogMessages();

    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1,
                  countLogLinesContaining("wrong type for opTime argument in replSetFresh "
                                          "response: int"));
}

TEST_F(FreshnessCheckerTest, ElectVetoed) {
    // other responds with veto
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1"))));

    std::vector<HostAndPort> hosts;
    hosts.push_back(config.getMemberAt(1).getHostAndPort());

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT_EQUALS(HostAndPort("h1"), noi->getRequest().target);
        _net->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            ResponseStatus(RemoteCommandResponse(
                BSON("ok" << 1 << "id" << 2 << "set"
                          << "rs0"
                          << "who"
                          << "h1"
                          << "cfgver"
                          << 1
                          << "veto"
                          << true
                          << "errmsg"
                          << "I'd rather you didn't"
                          << "opTime"
                          << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL())),
                BSONObj(),
                Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();

    stopCapturingLogMessages();

    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not electing self, h1:27017 would veto with "
                                          "'I'd rather you didn't'"));
}

int findIdForMember(const ReplicaSetConfig& rsConfig, const HostAndPort& host) {
    const MemberConfig* member = rsConfig.findMemberByHostAndPort(host);
    ASSERT_TRUE(member != NULL) << "No host named " << host.toString() << " in config";
    return member->getId();
}

TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestManyNodes) {
    // one other responds as fresher than us
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        hosts.push_back(mem->getHostAndPort());
    }

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                        << "rs0"
                        << "who" << target.toString() << "cfgver" << 1 << "opTime"
                        << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL());
        if (target.host() == "h1") {
            responseBuilder << "fresher" << true;
        }
        _net->scheduleResponse(noi,
                               startDate + Milliseconds(10),
                               ResponseStatus(RemoteCommandResponse(
                                   responseBuilder.obj(), BSONObj(), Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();
    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1, countLogLinesContaining("not electing self, we are not freshest"));
}

TEST_F(FreshnessCheckerTest, ElectNotElectingSelfWeAreNotFreshestOpTimeManyNodes) {
    // one other responds with a later optime than ours
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        if (HostAndPort("h0") == mem->getHostAndPort()) {
            continue;
        }
        hosts.push_back(mem->getHostAndPort());
    }

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();

    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        BSONObjBuilder responseBuilder;
        if (target.host() == "h4") {
            responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                            << "rs0"
                            << "who" << target.toString() << "cfgver" << 1 << "opTime"
                            << Date_t::fromMillisSinceEpoch(Timestamp(20, 0).asLL());
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(20),
                                   ResponseStatus(RemoteCommandResponse(
                                       responseBuilder.obj(), BSONObj(), Milliseconds(8))));
        } else {
            responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                            << "rs0"
                            << "who" << target.toString() << "cfgver" << 1 << "opTime"
                            << Date_t::fromMillisSinceEpoch(Timestamp(10, 0).asLL());
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(10),
                                   ResponseStatus(RemoteCommandResponse(
                                       responseBuilder.obj(), BSONObj(), Milliseconds(8))));
        }
    }
    _net->runUntil(startDate + Milliseconds(10));
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    ASSERT_EQUALS(0, countLogLinesContaining("not electing self, we are not freshest"));
    _net->runUntil(startDate + Milliseconds(20));
    ASSERT_EQUALS(startDate + Milliseconds(20), _net->now());
    _net->exitNetwork();
    waitOnChecker();
    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessCheckerTest, ElectWrongTypeInFreshnessResponseManyNodes) {
    // one other responds with "opTime" field of non-Date value, causing not freshest
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        hosts.push_back(mem->getHostAndPort());
    }

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                        << "rs0"
                        << "who" << target.toString() << "cfgver" << 1;
        if (target.host() == "h1") {
            responseBuilder << "opTime" << 3;
        } else {
            responseBuilder << "opTime" << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL());
        }
        _net->scheduleResponse(noi,
                               startDate + Milliseconds(10),
                               ResponseStatus(RemoteCommandResponse(
                                   responseBuilder.obj(), BSONObj(), Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();
    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1,
                  countLogLinesContaining("wrong type for opTime argument in replSetFresh "
                                          "response: int"));
}

TEST_F(FreshnessCheckerTest, ElectVetoedManyNodes) {
    // one other responds with veto
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        hosts.push_back(mem->getHostAndPort());
    }

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                        << "rs0"
                        << "who" << target.toString() << "cfgver" << 1 << "opTime"
                        << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL());
        if (target.host() == "h1") {
            responseBuilder << "veto" << true << "errmsg"
                            << "I'd rather you didn't";
        }
        _net->scheduleResponse(noi,
                               startDate + Milliseconds(10),
                               ResponseStatus(RemoteCommandResponse(
                                   responseBuilder.obj(), BSONObj(), Milliseconds(8))));
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();
    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not electing self, h1:27017 would veto with "
                                          "'I'd rather you didn't'"));
}

TEST_F(FreshnessCheckerTest, ElectVetoedAndTiedFreshnessManyNodes) {
    // one other responds with veto and another responds with tie
    startCapturingLogMessages();
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        if (HostAndPort("h0") == mem->getHostAndPort()) {
            continue;
        }
        hosts.push_back(mem->getHostAndPort());
    }

    const BSONObj freshRequest = makeFreshRequest(config, Timestamp(10, 0), 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();

    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        BSONObjBuilder responseBuilder;
        if (target.host() == "h4") {
            responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                            << "rs0"
                            << "who" << target.toString() << "cfgver" << 1 << "veto" << true
                            << "errmsg"
                            << "I'd rather you didn't"
                            << "opTime" << Date_t::fromMillisSinceEpoch(Timestamp(10, 0).asLL());
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(20),
                                   ResponseStatus(RemoteCommandResponse(
                                       responseBuilder.obj(), BSONObj(), Milliseconds(8))));
        } else {
            responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                            << "rs0"
                            << "who" << target.toString() << "cfgver" << 1 << "opTime"
                            << Date_t::fromMillisSinceEpoch(Timestamp(10, 0).asLL());
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(10),
                                   ResponseStatus(RemoteCommandResponse(
                                       responseBuilder.obj(), BSONObj(), Milliseconds(8))));
        }
    }
    _net->runUntil(startDate + Milliseconds(10));
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    ASSERT_EQUALS(0,
                  countLogLinesContaining("not electing self, h4:27017 would veto with '"
                                          "errmsg: \"I'd rather you didn't\"'"));
    _net->runUntil(startDate + Milliseconds(20));
    ASSERT_EQUALS(startDate + Milliseconds(20), _net->now());
    _net->exitNetwork();
    waitOnChecker();
    stopCapturingLogMessages();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
    ASSERT_EQUALS(1,
                  countLogLinesContaining("not electing self, h4:27017 would veto with "
                                          "'I'd rather you didn't'"));
}

TEST_F(FreshnessCheckerTest, ElectManyNodesNotAllRespond) {
    ReplicaSetConfig config = assertMakeRSConfig(BSON("_id"
                                                      << "rs0"
                                                      << "version"
                                                      << 1
                                                      << "members"
                                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                               << "h0")
                                                                    << BSON("_id" << 2 << "host"
                                                                                  << "h1")
                                                                    << BSON("_id" << 3 << "host"
                                                                                  << "h2")
                                                                    << BSON("_id" << 4 << "host"
                                                                                  << "h3")
                                                                    << BSON("_id" << 5 << "host"
                                                                                  << "h4"))));

    std::vector<HostAndPort> hosts;
    for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin(); mem != config.membersEnd();
         ++mem) {
        hosts.push_back(mem->getHostAndPort());
    }

    const Timestamp lastOpTimeApplied(10, 0);
    const BSONObj freshRequest = makeFreshRequest(config, lastOpTimeApplied, 0);

    startTest(Timestamp(10, 0), config, 0, hosts);
    const Date_t startDate = _net->now();
    unordered_set<HostAndPort> seen;
    _net->enterNetwork();
    for (size_t i = 0; i < hosts.size(); ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = _net->getNextReadyRequest();
        const HostAndPort target = noi->getRequest().target;
        ASSERT_EQUALS("admin", noi->getRequest().dbname);
        ASSERT_EQUALS(freshRequest, noi->getRequest().cmdObj);
        ASSERT(seen.insert(target).second) << "Already saw " << target;
        if (target.host() == "h2" || target.host() == "h3") {
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(10),
                                   ResponseStatus(ErrorCodes::NoSuchKey, "No response"));
        } else {
            BSONObjBuilder responseBuilder;
            responseBuilder << "ok" << 1 << "id" << findIdForMember(config, target) << "set"
                            << "rs0"
                            << "who" << target.toString() << "cfgver" << 1 << "opTime"
                            << Date_t::fromMillisSinceEpoch(Timestamp(0, 0).asLL());
            _net->scheduleResponse(noi,
                                   startDate + Milliseconds(10),
                                   ResponseStatus(RemoteCommandResponse(
                                       responseBuilder.obj(), BSONObj(), Milliseconds(8))));
        }
    }
    _net->runUntil(startDate + Milliseconds(10));
    _net->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), _net->now());
    waitOnChecker();
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::None);
}

class FreshnessScatterGatherTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        int selfConfigIndex = 0;
        Timestamp lastOpTimeApplied(100, 0);

        ReplicaSetConfig config;
        config.initialize(BSON("_id"
                               << "rs0"
                               << "version"
                               << 1
                               << "members"
                               << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                        << "host0")
                                             << BSON("_id" << 1 << "host"
                                                           << "host1")
                                             << BSON("_id" << 2 << "host"
                                                           << "host2"))));

        std::vector<HostAndPort> hosts;
        for (ReplicaSetConfig::MemberIterator mem = ++config.membersBegin();
             mem != config.membersEnd();
             ++mem) {
            hosts.push_back(mem->getHostAndPort());
        }

        _checker.reset(
            new FreshnessChecker::Algorithm(lastOpTimeApplied, config, selfConfigIndex, hosts));
    }

    virtual void tearDown() {
        _checker.reset(NULL);
    }

protected:
    bool hasReceivedSufficientResponses() {
        return _checker->hasReceivedSufficientResponses();
    }

    void processResponse(const RemoteCommandRequest& request, const ResponseStatus& response) {
        _checker->processResponse(request, response);
    }

    FreshnessChecker::ElectionAbortReason shouldAbortElection() const {
        return _checker->shouldAbortElection();
    }

    ResponseStatus lessFresh() {
        BSONObjBuilder bb;
        bb.append("ok", 1.0);
        bb.appendDate("opTime", Date_t::fromMillisSinceEpoch(Timestamp(10, 0).asLL()));
        return ResponseStatus(
            NetworkInterfaceMock::Response(bb.obj(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus moreFreshViaOpTime() {
        BSONObjBuilder bb;
        bb.append("ok", 1.0);
        bb.appendDate("opTime", Date_t::fromMillisSinceEpoch(Timestamp(110, 0).asLL()));
        return ResponseStatus(
            NetworkInterfaceMock::Response(bb.obj(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus wrongTypeForOpTime() {
        BSONObjBuilder bb;
        bb.append("ok", 1.0);
        bb.append("opTime", std::string("several minutes ago"));
        return ResponseStatus(
            NetworkInterfaceMock::Response(bb.obj(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus unauthorized() {
        BSONObjBuilder bb;
        bb.append("ok", 0.0);
        bb.append("code", ErrorCodes::Unauthorized);
        bb.append("errmsg", "Unauthorized");
        return ResponseStatus(
            NetworkInterfaceMock::Response(bb.obj(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus tiedForFreshness() {
        BSONObjBuilder bb;
        bb.append("ok", 1.0);
        bb.appendDate("opTime", Date_t::fromMillisSinceEpoch(Timestamp(100, 0).asLL()));
        return ResponseStatus(
            NetworkInterfaceMock::Response(bb.obj(), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus moreFresh() {
        return ResponseStatus(NetworkInterfaceMock::Response(
            BSON("ok" << 1.0 << "fresher" << true), BSONObj(), Milliseconds(10)));
    }

    ResponseStatus veto() {
        return ResponseStatus(
            NetworkInterfaceMock::Response(BSON("ok" << 1.0 << "veto" << true << "errmsg"
                                                     << "vetoed!"),
                                           BSONObj(),
                                           Milliseconds(10)));
    }

    RemoteCommandRequest requestFrom(std::string hostname) {
        return RemoteCommandRequest(HostAndPort(hostname),
                                    "",  // the non-hostname fields do not matter in Freshness
                                    BSONObj(),
                                    Milliseconds(0));
    }

private:
    unique_ptr<FreshnessChecker::Algorithm> _checker;
};

TEST_F(FreshnessScatterGatherTest, BothNodesLessFresh) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), lessFresh());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), lessFresh());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::None);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeFresher) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), moreFresh());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeFresherViaOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), moreFreshViaOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeVetoes) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), veto());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeWrongTypeForOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), wrongTypeForOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeTiedForFreshness) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), tiedForFreshness());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), lessFresh());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FreshnessTie);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeTiedAndSecondFresher) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), tiedForFreshness());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), moreFresh());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeTiedAndSecondFresherViaOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), tiedForFreshness());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), moreFreshViaOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeTiedAndSecondVetoes) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), tiedForFreshness());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), veto());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeTiedAndSecondWrongTypeForOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), tiedForFreshness());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), wrongTypeForOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, FirstNodeLessFreshAndSecondWrongTypeForOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), lessFresh());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), wrongTypeForOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, SecondNodeTiedAndFirstWrongTypeForOpTime) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), wrongTypeForOpTime());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::FresherNodeFound);
}

TEST_F(FreshnessScatterGatherTest, NotEnoughVotersDueNetworkErrors) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), ResponseStatus(Status(ErrorCodes::NetworkTimeout, "")));
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), ResponseStatus(Status(ErrorCodes::NetworkTimeout, "")));
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::QuorumUnreachable);
}

TEST_F(FreshnessScatterGatherTest, NotEnoughVotersDueToUnauthorized) {
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host1"), unauthorized());
    ASSERT_FALSE(hasReceivedSufficientResponses());

    processResponse(requestFrom("host2"), unauthorized());
    ASSERT_TRUE(hasReceivedSufficientResponses());
    ASSERT_EQUALS(shouldAbortElection(), FreshnessChecker::QuorumUnreachable);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
