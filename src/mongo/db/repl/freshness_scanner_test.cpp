/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/db/repl/freshness_scanner.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using unittest::assertGet;

class FreshnessScannerTest : public mongo::unittest::Test {
public:
    NetworkInterfaceMock* getNet() {
        return _net;
    }
    ReplicationExecutor* getExecutor() {
        return _executor.get();
    }

    virtual void setUp() {
        ASSERT_OK(_config.initialize(BSON("_id"
                                          << "rs0"
                                          << "version"
                                          << 1
                                          << "members"
                                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                   << "host0")
                                                        << BSON("_id" << 1 << "host"
                                                                      << "host1")
                                                        << BSON("_id" << 2 << "host"
                                                                      << "host2")
                                                        << BSON("_id" << 3 << "host"
                                                                      << "host3"
                                                                      << "votes"
                                                                      << 0
                                                                      << "priority"
                                                                      << 0)
                                                        << BSON("_id" << 4 << "host"
                                                                      << "host4"
                                                                      << "votes"
                                                                      << 0
                                                                      << "priority"
                                                                      << 0)))));
        ASSERT_OK(_config.validate());

        _net = new NetworkInterfaceMock;
        _executor = stdx::make_unique<ReplicationExecutor>(_net, 1 /* prng seed */);
        _executorThread =
            stdx::make_unique<stdx::thread>(stdx::bind(&ReplicationExecutor::run, _executor.get()));
    }

    virtual void tearDown() {
        _executor->shutdown();
        _executorThread->join();
    }

protected:
    RemoteCommandRequest requestFrom(std::string hostname) {
        return RemoteCommandRequest(HostAndPort(hostname),
                                    "",  // fields do not matter in FreshnessScanner
                                    BSONObj(),
                                    Milliseconds(0));
    }

    ResponseStatus makeResponseStatus(BSONObj response) {
        return ResponseStatus(
            NetworkInterfaceMock::Response(response, BSONObj(), Milliseconds(10)));
    }

    ResponseStatus badResponseStatus() {
        return ResponseStatus(ErrorCodes::NodeNotFound, "not on my watch");
    }

    ResponseStatus goodResponseStatus(Timestamp timestamp, long long term) {
        // OpTime part of replSetGetStatus.
        BSONObj response =
            BSON("optimes" << BSON("appliedOpTime" << OpTime(timestamp, term).toBSON()));
        return makeResponseStatus(response);
    }

    ReplicaSetConfig _config;

private:
    // owned by _executor
    NetworkInterfaceMock* _net;
    std::unique_ptr<ReplicationExecutor> _executor;
    std::unique_ptr<stdx::thread> _executorThread;
};

TEST_F(FreshnessScannerTest, ImmediateGoodResponse) {
    FreshnessScanner::Algorithm algo(_config, 0, Milliseconds(2000));

    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host1"), goodResponseStatus(Timestamp(1, 100), 1));
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host2"), goodResponseStatus(Timestamp(1, 200), 1));
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host3"), goodResponseStatus(Timestamp(1, 400), 1));
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host4"), goodResponseStatus(Timestamp(1, 300), 1));
    ASSERT_TRUE(algo.hasReceivedSufficientResponses());
    ASSERT_EQUALS((size_t)4, algo.getResult().size());
    ASSERT_EQUALS(3, algo.getResult().front().index);
    ASSERT_EQUALS(OpTime(Timestamp(1, 400), 1), algo.getResult().front().opTime);
    ASSERT_EQUALS(1, algo.getResult().back().index);
    ASSERT_EQUALS(OpTime(Timestamp(1, 100), 1), algo.getResult().back().opTime);
}

TEST_F(FreshnessScannerTest, ImmediateBadResponse) {
    FreshnessScanner::Algorithm algo(_config, 0, Milliseconds(2000));

    // Cannot access host 1 and host 2.
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host1"), badResponseStatus());
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());
    algo.processResponse(requestFrom("host2"), badResponseStatus());
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());

    // host 3 is in an old version, which doesn't include OpTimes in the response.
    algo.processResponse(requestFrom("host3"), makeResponseStatus(BSONObj()));
    ASSERT_FALSE(algo.hasReceivedSufficientResponses());

    // Responses from host 4 in PV0 are considered as bad responses.
    auto response4 = BSON("optimes" << BSON("appliedOpTime" << Timestamp(1, 300)));
    algo.processResponse(requestFrom("host4"), makeResponseStatus(response4));
    ASSERT_TRUE(algo.hasReceivedSufficientResponses());
    ASSERT_EQUALS((size_t)0, algo.getResult().size());
}

TEST_F(FreshnessScannerTest, AllResponsesTimeout) {
    Milliseconds timeout(2000);
    FreshnessScanner scanner;
    scanner.start(getExecutor(), _config, 0, timeout);

    auto net = getNet();
    net->enterNetwork();
    ASSERT(net->hasReadyRequests());
    // Black hole all requests.
    while (net->hasReadyRequests()) {
        net->blackHole(net->getNextReadyRequest());
    }
    auto later = net->now() + Milliseconds(2010);
    ASSERT_EQ(later, net->runUntil(later));
    net->exitNetwork();
    ASSERT_EQUALS((size_t)0, scanner.getResult().size());
}


TEST_F(FreshnessScannerTest, BadResponsesAndTimeout) {
    Milliseconds timeout(2000);
    FreshnessScanner scanner;
    scanner.start(getExecutor(), _config, 0, timeout);

    auto net = getNet();
    net->enterNetwork();

    Date_t later = net->now() + Milliseconds(10);
    // host 1 returns good response.
    ASSERT(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    HostAndPort successfulHost = noi->getRequest().target;
    net->scheduleResponse(noi, later, goodResponseStatus(Timestamp(1, 100), 1));

    // host 2 has a bad connection.
    ASSERT(net->hasReadyRequests());
    net->scheduleResponse(net->getNextReadyRequest(), later, badResponseStatus());

    // host 3 and 4 time out.
    ASSERT(net->hasReadyRequests());
    net->blackHole(net->getNextReadyRequest());
    ASSERT(net->hasReadyRequests());
    net->blackHole(net->getNextReadyRequest());

    // Advance the clock.
    ASSERT(!net->hasReadyRequests());
    getNet()->runUntil(getNet()->now() + Milliseconds(2010));
    getNet()->exitNetwork();

    ASSERT_EQUALS((size_t)1, scanner.getResult().size());
    auto freshnessInfo = scanner.getResult().front();
    ASSERT_EQUALS(_config.findMemberIndexByHostAndPort(successfulHost), freshnessInfo.index);
    ASSERT_EQUALS(OpTime(Timestamp(1, 100), 1), freshnessInfo.opTime);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
