/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class MockProgressManager {
public:
    void updateMap(int memberId, const OpTime& lastDurableOpTime, const OpTime& lastAppliedOpTime) {
        progressMap[memberId] = ProgressInfo(lastDurableOpTime, lastAppliedOpTime);
    }

    void setResult(bool newResult) {
        _result = newResult;
    }

    bool prepareReplSetUpdatePositionCommand(BSONObjBuilder* cmdBuilder) {
        if (!_result) {
            return _result;
        }
        cmdBuilder->append("replSetUpdatePosition", 1);
        BSONArrayBuilder arrayBuilder(cmdBuilder->subarrayStart("optimes"));
        for (auto&& itr : progressMap) {
            BSONObjBuilder entry(arrayBuilder.subobjStart());
            itr.second.lastDurableOpTime.append(&entry, "durableOpTime");
            itr.second.lastAppliedOpTime.append(&entry, "appliedOpTime");
            entry.append("memberId", itr.first);
            entry.append("cfgver", 1);
        }
        return true;
    }

private:
    struct ProgressInfo {
        ProgressInfo() = default;
        ProgressInfo(const OpTime& lastDurableOpTime, const OpTime& lastAppliedOpTime)
            : lastDurableOpTime(lastDurableOpTime), lastAppliedOpTime(lastAppliedOpTime) {}

        // Our last known OpTime that this slave has applied and journaled to.
        OpTime lastDurableOpTime;
        // Our last known OpTime that this slave has applied, whether journaled or unjournaled.
        OpTime lastAppliedOpTime;
    };

    std::map<int, ProgressInfo> progressMap;
    bool _result = true;
};

class ReporterTest : public executor::ThreadPoolExecutorTest {
public:
    ReporterTest();
    void scheduleNetworkResponse(const BSONObj& obj);
    void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason);

protected:
    void setUp() override;
    void tearDown() override;

    std::unique_ptr<Reporter> reporter;
    std::unique_ptr<MockProgressManager> posUpdater;
    Reporter::PrepareReplSetUpdatePositionCommandFn prepareReplSetUpdatePositionCommandFn;
};

ReporterTest::ReporterTest() {}

void ReporterTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    posUpdater.reset(new MockProgressManager());
    prepareReplSetUpdatePositionCommandFn = [this]() -> StatusWith<BSONObj> {
        BSONObjBuilder bob;
        if (posUpdater->prepareReplSetUpdatePositionCommand(&bob)) {
            return bob.obj();
        }
        return Status(ErrorCodes::OperationFailed,
                      "unable to prepare replSetUpdatePosition command object");
    };
    reporter.reset(new Reporter(&getExecutor(),
                                [this]() { return prepareReplSetUpdatePositionCommandFn(); },
                                HostAndPort("h1")));
    launchExecutorThread();
}

void ReporterTest::tearDown() {
    executor::ThreadPoolExecutorTest::tearDown();
    // Executor may still invoke reporter's callback before shutting down.
    posUpdater.reset();
    reporter.reset();
}

void ReporterTest::scheduleNetworkResponse(const BSONObj& obj) {
    auto net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    Milliseconds millis(0);
    RemoteCommandResponse response(obj, BSONObj(), millis);
    executor::TaskExecutor::ResponseStatus responseStatus(response);
    net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
}

void ReporterTest::scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
    auto net = getNet();
    ASSERT_TRUE(net->hasReadyRequests());
    executor::TaskExecutor::ResponseStatus responseStatus(code, reason);
    net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
}

TEST_F(ReporterTest, InvalidConstruction) {
    // null PrepareReplSetUpdatePositionCommandFn
    ASSERT_THROWS(Reporter(&getExecutor(),
                           Reporter::PrepareReplSetUpdatePositionCommandFn(),
                           HostAndPort("h1")),
                  UserException);

    // null TaskExecutor
    ASSERT_THROWS(Reporter(nullptr, prepareReplSetUpdatePositionCommandFn, HostAndPort("h1")),
                  UserException);

    // empty HostAndPort
    ASSERT_THROWS(Reporter(&getExecutor(), prepareReplSetUpdatePositionCommandFn, HostAndPort()),
                  UserException);
}

TEST_F(ReporterTest, IsActiveOnceScheduled) {
    ASSERT_FALSE(reporter->isActive());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
}

TEST_F(ReporterTest, CancelWithoutScheduled) {
    ASSERT_FALSE(reporter->isActive());
    reporter->cancel();
    ASSERT_FALSE(reporter->isActive());
}

TEST_F(ReporterTest, ShutdownBeforeSchedule) {
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, reporter->trigger());
    ASSERT_FALSE(reporter->isActive());
}

// If an error is returned, it should be recorded in the Reporter and be returned when triggered
TEST_F(ReporterTest, ErrorsAreStoredInTheReporter) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(ErrorCodes::NoSuchKey, "waaaah");
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, reporter->getStatus());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, reporter->trigger());
    ASSERT_FALSE(reporter->isActive());
}

// If an error is returned, it should be recorded in the Reporter and not run again.
TEST_F(ReporterTest, ErrorsStopTheReporter) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(ErrorCodes::NoSuchKey, "waaaah");
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, reporter->getStatus());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_FALSE(reporter->isActive());
}

// Schedule while we are already scheduled, it should set willRunAgain, then automatically
// schedule itself after finishing.
TEST_F(ReporterTest, DoubleScheduleShouldCauseRescheduleImmediatelyAfterRespondedTo) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());

    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_FALSE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
}

// Schedule multiple times while we are already scheduled, it should set willRunAgain,
// then automatically schedule itself after finishing, but not a third time since the latter
// two will contain the same batch of updates.
TEST_F(ReporterTest, TripleScheduleShouldCauseRescheduleImmediatelyAfterRespondedToOnlyOnce) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());

    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_FALSE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
}

TEST_F(ReporterTest, CancelWhileScheduled) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());

    reporter->cancel();

    auto net = getNet();
    net->enterNetwork();
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_FALSE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->getStatus());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->trigger());
}

TEST_F(ReporterTest, CancelAfterFirstReturns) {
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->willRunAgain());

    auto net = getNet();
    net->enterNetwork();
    scheduleNetworkResponse(BSON("ok" << 1));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());

    reporter->cancel();
    net->enterNetwork();
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_FALSE(reporter->isActive());
    ASSERT_FALSE(reporter->willRunAgain());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->getStatus());

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->trigger());
}

TEST_F(ReporterTest, ProgressManagerFails) {
    posUpdater->setResult(false);
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, reporter->trigger().code());
}

}  // namespace
