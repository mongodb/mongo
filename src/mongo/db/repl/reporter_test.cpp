/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/repl/reporter.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <map>
#include <memory>
#include <ratio>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace {

using namespace mongo;
using namespace mongo::repl;
using executor::RemoteCommandResponse;

class MockProgressManager {
public:
    void updateMap(int memberId, const OpTime& lastDurableOpTime, const OpTime& lastAppliedOpTime) {
        _progressMap[memberId] = ProgressInfo(lastDurableOpTime, lastAppliedOpTime);
    }

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() {
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append(UpdatePositionArgs::kCommandFieldName, 1);
        BSONArrayBuilder arrayBuilder(
            cmdBuilder.subarrayStart(UpdatePositionArgs::kUpdateArrayFieldName));
        for (auto&& itr : _progressMap) {
            BSONObjBuilder entry(arrayBuilder.subobjStart());
            itr.second.lastDurableOpTime.append(UpdatePositionArgs::kDurableOpTimeFieldName,
                                                &entry);
            entry.appendDate(UpdatePositionArgs::kDurableWallTimeFieldName,
                             Date_t() + Seconds(itr.second.lastDurableOpTime.getSecs()));
            itr.second.lastAppliedOpTime.append(UpdatePositionArgs::kAppliedOpTimeFieldName,
                                                &entry);
            entry.appendDate(UpdatePositionArgs::kAppliedWallTimeFieldName,
                             Date_t() + Seconds(itr.second.lastAppliedOpTime.getSecs()));
            entry.append(UpdatePositionArgs::kMemberIdFieldName, itr.first);
            if (_configVersion != -1) {
                entry.append(UpdatePositionArgs::kConfigVersionFieldName, _configVersion);
            }
        }
        arrayBuilder.done();
        return cmdBuilder.obj();
    }

private:
    struct ProgressInfo {
        ProgressInfo() = default;
        ProgressInfo(const OpTime& lastDurableOpTime, const OpTime& lastAppliedOpTime)
            : lastDurableOpTime(lastDurableOpTime), lastAppliedOpTime(lastAppliedOpTime) {}

        // Our last known OpTime that this secondary has applied and journaled to.
        OpTime lastDurableOpTime;
        // Our last known OpTime that this secondary has applied, whether journaled or unjournaled.
        OpTime lastAppliedOpTime;
    };

    std::map<int, ProgressInfo> _progressMap;
    long long _configVersion = 1;
};

class ReporterTest : public executor::ThreadPoolExecutorTest {
public:
    ReporterTest();

    /**
     * Schedules network response and instructs network interface to process response.
     * Returns command object in the network request.
     */
    BSONObj processNetworkResponse(const BSONObj& obj,
                                   bool expectReadyRequestsAfterProcessing = false);
    BSONObj processNetworkResponse(RemoteCommandResponse rs,
                                   bool expectReadyRequestsAfterProcessing = false);

    void runUntil(Date_t when, bool expectReadyRequestsAfterAdvancingClock = false);

    void runReadyScheduledTasks();

    void assertReporterDone();

protected:
    void setUp() override;
    void tearDown() override;

private:
    virtual bool triggerAtSetUp() const;

protected:
    std::unique_ptr<unittest::TaskExecutorProxy> _executorProxy;
    std::unique_ptr<MockProgressManager> posUpdater;
    Reporter::PrepareReplSetUpdatePositionCommandFn prepareReplSetUpdatePositionCommandFn;
    std::unique_ptr<Reporter> reporter;
};

class ReporterTestNoTriggerAtSetUp : public ReporterTest {
private:
    bool triggerAtSetUp() const override;
};

ReporterTest::ReporterTest() {}

void ReporterTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    _executorProxy = std::make_unique<unittest::TaskExecutorProxy>(&getExecutor());

    posUpdater = std::make_unique<MockProgressManager>();
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));

    prepareReplSetUpdatePositionCommandFn = [updater = posUpdater.get()] {
        return updater->prepareReplSetUpdatePositionCommand();
    };

    reporter = std::make_unique<Reporter>(
        _executorProxy.get(),
        [this]() { return prepareReplSetUpdatePositionCommandFn(); },
        HostAndPort("h1"),
        Milliseconds(1000),
        Milliseconds(5000));
    launchExecutorThread();

    if (triggerAtSetUp()) {
        ASSERT_OK(reporter->trigger());
        ASSERT_TRUE(reporter->isActive());
    } else {
        ASSERT_FALSE(reporter->isActive());
    }
    ASSERT_FALSE(reporter->isWaitingToSendReport());
}

void ReporterTest::tearDown() {
    shutdownExecutorThread();
    joinExecutorThread();
    // Executor may still invoke reporter's callback before shutting down.
    reporter.reset();
    posUpdater.reset();
    prepareReplSetUpdatePositionCommandFn = Reporter::PrepareReplSetUpdatePositionCommandFn();
    _executorProxy.reset();
}

bool ReporterTest::triggerAtSetUp() const {
    return true;
}

bool ReporterTestNoTriggerAtSetUp::triggerAtSetUp() const {
    return false;
}

BSONObj ReporterTest::processNetworkResponse(const BSONObj& obj,
                                             bool expectReadyRequestsAfterProcessing) {
    auto net = getNet();
    net->enterNetwork();
    auto cmdObj = net->scheduleSuccessfulResponse(obj).cmdObj;
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(expectReadyRequestsAfterProcessing, net->hasReadyRequests());
    net->exitNetwork();
    return cmdObj;
}

BSONObj ReporterTest::processNetworkResponse(RemoteCommandResponse rs,
                                             bool expectReadyRequestsAfterProcessing) {
    auto net = getNet();
    net->enterNetwork();
    auto cmdObj = net->scheduleErrorResponse(rs).cmdObj;
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(expectReadyRequestsAfterProcessing, net->hasReadyRequests());
    net->exitNetwork();
    return cmdObj;
}

void ReporterTest::runUntil(Date_t until, bool expectReadyRequestsAfterAdvancingClock) {
    auto net = getNet();
    net->enterNetwork();
    ASSERT_EQUALS(until, net->runUntil(until));
    ASSERT_EQUALS(expectReadyRequestsAfterAdvancingClock, net->hasReadyRequests());
    net->exitNetwork();
}

void ReporterTest::runReadyScheduledTasks() {
    auto net = getNet();
    net->enterNetwork();
    net->exitNetwork();
}

void ReporterTest::assertReporterDone() {
    ASSERT_FALSE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());
    ASSERT_EQUALS(Date_t(), reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_EQUALS(reporter->join(), reporter->trigger());
}

TEST(UpdatePositionArgs, AcceptsUnknownField) {
    UpdatePositionArgs updatePositionArgs;
    BSONObjBuilder bob;
    bob.append(UpdatePositionArgs::kCommandFieldName, 1);
    bob.append(UpdatePositionArgs::kUpdateArrayFieldName, BSONArray());
    bob.append("unknownField", 1);  // append an unknown field.
    BSONObj cmdObj = bob.obj();
    ASSERT_OK(updatePositionArgs.initialize(cmdObj));
}
TEST(UpdatePositionArgs, AcceptsUnknownFieldInUpdateInfo) {
    UpdatePositionArgs updatePositionArgs;
    BSONObjBuilder bob;
    bob.append(UpdatePositionArgs::kCommandFieldName, 1);
    auto now = Date_t();
    auto updateInfo =
        BSON(UpdatePositionArgs::kConfigVersionFieldName
             << 1 << UpdatePositionArgs::kMemberIdFieldName << 1
             << UpdatePositionArgs::kAppliedOpTimeFieldName << OpTime()
             << UpdatePositionArgs::kWrittenOpTimeFieldName << OpTime()
             << UpdatePositionArgs::kDurableOpTimeFieldName << OpTime()
             << UpdatePositionArgs::kAppliedWallTimeFieldName << now
             << UpdatePositionArgs::kWrittenWallTimeFieldName << now
             << UpdatePositionArgs::kDurableWallTimeFieldName << now << "unknownField" << 1);
    bob.append(UpdatePositionArgs::kUpdateArrayFieldName, BSON_ARRAY(updateInfo));
    BSONObj cmdObj = bob.obj();
    ASSERT_OK(updatePositionArgs.initialize(cmdObj));

    // The serialized object should be the same as the original except for the unknown field.
    BSONObjBuilder bob2;
    auto updateArgsObj = updatePositionArgs.toBSON();
    auto updatesArr =
        BSONArray(updateArgsObj.getObjectField(UpdatePositionArgs::kUpdateArrayFieldName));
    ASSERT_EQ(updatesArr.nFields(), 1);
    bob2.appendElements(updatesArr[0].Obj());
    bob2.append(UpdatePositionArgs::kAppliedWallTimeFieldName, now);
    bob2.append(UpdatePositionArgs::kWrittenWallTimeFieldName, now);
    bob2.append(UpdatePositionArgs::kDurableWallTimeFieldName, now);
    bob2.append("unknownField", 1);
    ASSERT_EQ(bob2.obj().woCompare(updateInfo), 0);
}

TEST_F(ReporterTestNoTriggerAtSetUp, InvalidConstruction) {
    // null PrepareReplSetUpdatePositionCommandFn
    ASSERT_THROWS(Reporter(&getExecutor(),
                           Reporter::PrepareReplSetUpdatePositionCommandFn(),
                           HostAndPort("h1"),
                           Milliseconds(1000),
                           Milliseconds(5000)),
                  AssertionException);

    // null TaskExecutor
    ASSERT_THROWS_WHAT(Reporter(nullptr,
                                prepareReplSetUpdatePositionCommandFn,
                                HostAndPort("h1"),
                                Milliseconds(1000),
                                Milliseconds(5000)),
                       AssertionException,
                       "null task executor");

    // null PrepareReplSetUpdatePositionCommandFn
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                Reporter::PrepareReplSetUpdatePositionCommandFn(),
                                HostAndPort("h1"),
                                Milliseconds(1000),
                                Milliseconds(5000)),
                       AssertionException,
                       "null function to create replSetUpdatePosition command object");

    // empty HostAndPort
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                prepareReplSetUpdatePositionCommandFn,
                                HostAndPort(),
                                Milliseconds(1000),
                                Milliseconds(5000)),
                       AssertionException,
                       "target name cannot be empty");

    // zero keep alive interval.
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                prepareReplSetUpdatePositionCommandFn,
                                HostAndPort("h1"),
                                Seconds(-1),
                                Milliseconds(5000)),
                       AssertionException,
                       "keep alive interval must be positive");

    // negative keep alive interval.
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                prepareReplSetUpdatePositionCommandFn,
                                HostAndPort("h1"),
                                Seconds(-1),
                                Milliseconds(5000)),
                       AssertionException,
                       "keep alive interval must be positive");
}

TEST_F(ReporterTestNoTriggerAtSetUp, GetTarget) {
    ASSERT_EQUALS(HostAndPort("h1"), reporter->getTarget());
}

TEST_F(ReporterTestNoTriggerAtSetUp, IsActiveOnceScheduled) {
    ASSERT_FALSE(reporter->isActive());
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
}

TEST_F(ReporterTestNoTriggerAtSetUp, ShutdownWithoutScheduledStopsTheReporter) {
    ASSERT_FALSE(reporter->isActive());
    reporter->shutdown();
    Status expectedStatus(ErrorCodes::CallbackCanceled, "Reporter no longer valid");
    ASSERT_EQUALS(expectedStatus, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTestNoTriggerAtSetUp,
       ShuttingExecutorDownBeforeActivatingReporterPreventsTheReporterFromStarting) {
    getExecutor().shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, reporter->trigger());
    assertReporterDone();
}

TEST_F(ReporterTestNoTriggerAtSetUp, IsNotActiveAfterUpdatePositionTimeoutExpires) {

    auto prepareReplSetUpdatePositionCommandFn = [updater = posUpdater.get()] {
        return updater->prepareReplSetUpdatePositionCommand();
    };

    // Create a new test Reporter so we can configure the update position timeout.
    Milliseconds updatePositionTimeout = Milliseconds(5000);
    Reporter testReporter(&getExecutor(),
                          prepareReplSetUpdatePositionCommandFn,
                          HostAndPort("h1"),
                          Milliseconds(1000),
                          updatePositionTimeout);

    ASSERT_OK(testReporter.trigger());
    ASSERT_TRUE(testReporter.isActive());

    auto net = getNet();
    net->enterNetwork();

    // Schedule a response to the updatePosition command at a time that exceeds the timeout. Then
    // make sure the reporter shut down due to a remote command timeout.
    auto updatePosRequest = net->getNextReadyRequest();
    RemoteCommandResponse response =
        RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(0));
    executor::TaskExecutor::ResponseStatus responseStatus(response);
    net->scheduleResponse(
        updatePosRequest, net->now() + updatePositionTimeout + Milliseconds(1), responseStatus);
    net->runUntil(net->now() + updatePositionTimeout + Milliseconds(1));
    net->runReadyNetworkOperations();
    net->exitNetwork();

    // Reporter should have shut down.
    ASSERT_FALSE(testReporter.isWaitingToSendReport());
    ASSERT_FALSE(testReporter.isActive());
    ASSERT_TRUE(ErrorCodes::isExceededTimeLimitError(testReporter.getStatus_forTest().code()));
}

// If an error is returned, it should be recorded in the Reporter and not run again.
TEST_F(ReporterTest, TaskExecutorAndNetworkErrorsStopTheReporter) {
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::NoSuchKey, "waaaah"), Milliseconds(0)));

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, UnsuccessfulCommandResponseStopsTheReporter) {
    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::UnknownError) << "errmsg"
                                     << "unknown error"));

    ASSERT_EQUALS(Status(ErrorCodes::UnknownError, "unknown error"), reporter->join());
    assertReporterDone();
}

// Schedule while we are already scheduled, it should set "isWaitingToSendReport", then
// automatically
// schedule itself after finishing.
TEST_F(
    ReporterTest,
    TriggeringReporterOnceWhileFirstCommandRequestIsInProgressCausesSecondCommandRequestToBeSentImmediatelyAfterFirstResponseReturns) {
    // Second trigger (first time in setUp).
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1), true);

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1));

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());
}

// Schedule multiple times while we are already scheduled, it should set "isWaitingToSendReport",
// then automatically schedule itself after finishing, but not a third time since the latter
// two will contain the same batch of updates.
TEST_F(
    ReporterTest,
    TriggeringReporterTwiceWhileFirstCommandRequestIsInProgressCausesSecondCommandRequestToBeSentImmediatelyAfterFirstResponseReturns) {
    // Second trigger (first time in setUp).
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    // Third trigger.
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1), true);

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1));

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());
}

TEST_F(ReporterTest, ShuttingReporterDownWhileFirstCommandRequestIsInProgressStopsTheReporter) {
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    reporter->shutdown();

    auto net = getNet();
    net->enterNetwork();
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, ShuttingReporterDownWhileSecondCommandRequestIsInProgressStopsTheReporter) {
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1), true);

    ASSERT_TRUE(reporter->isActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());
    reporter->shutdown();

    auto net = getNet();
    net->enterNetwork();
    net->runReadyNetworkOperations();
    ASSERT_FALSE(net->hasReadyRequests());
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTestNoTriggerAtSetUp, CommandPreparationFailureStopsTheReporter) {
    Status expectedStatus(ErrorCodes::UnknownError, "unknown error");
    prepareReplSetUpdatePositionCommandFn = [expectedStatus]() -> StatusWith<BSONObj> {
        return expectedStatus;
    };
    ASSERT_OK(reporter->trigger());

    ASSERT_EQUALS(expectedStatus, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, CommandPreparationFailureDuringRescheduleStopsTheReporter) {
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    runReadyScheduledTasks();

    // This will cause command preparation to fail for the subsequent request.
    Status expectedStatus(ErrorCodes::UnknownError, "unknown error");
    prepareReplSetUpdatePositionCommandFn = [expectedStatus]() -> StatusWith<BSONObj> {
        return expectedStatus;
    };

    processNetworkResponse(BSON("ok" << 1));

    ASSERT_EQUALS(expectedStatus, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, FailedUpdateShouldNotRescheduleUpdate) {
    processNetworkResponse(RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::OperationFailed, "update failed"), Milliseconds(0)));

    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, SuccessfulUpdateShouldRescheduleUpdate) {
    processNetworkResponse(BSON("ok" << 1));

    auto until = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(until, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    runUntil(until, true);

    processNetworkResponse(RemoteCommandResponse::make_forTest(
        Status(ErrorCodes::OperationFailed, "update failed"), Milliseconds(0)));

    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, ShutdownWhileKeepAliveTimeoutIsScheduledShouldMakeReporterInactive) {
    processNetworkResponse(BSON("ok" << 1));

    auto until = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(until, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    reporter->shutdown();
    runReadyNetworkOperations();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    ASSERT_FALSE(reporter->isActive());

    runUntil(until);
}

TEST_F(ReporterTestNoTriggerAtSetUp,
       FailingToSchedulePrepareCommandTaskShouldMakeReporterInactive) {
    class TaskExecutorWithFailureInScheduleWork : public unittest::TaskExecutorProxy {
    public:
        using unittest::TaskExecutorProxy::TaskExecutorProxy;

        StatusWith<executor::TaskExecutor::CallbackHandle> scheduleWork(
            CallbackFn&& override) override {
            return Status(ErrorCodes::OperationFailed, "failed to schedule work");
        }
    };

    auto badExecutor = std::make_shared<TaskExecutorWithFailureInScheduleWork>(&getExecutor());
    _executorProxy->setExecutor(badExecutor.get());

    auto status = reporter->trigger();

    _executorProxy->setExecutor(&getExecutor());

    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    ASSERT_FALSE(reporter->isActive());
}

TEST_F(ReporterTestNoTriggerAtSetUp, FailingToScheduleRemoteCommandTaskShouldMakeReporterInactive) {
    class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
    public:
        using unittest::TaskExecutorProxy::TaskExecutorProxy;

        StatusWith<executor::TaskExecutor::CallbackHandle> scheduleRemoteCommand(
            const executor::RemoteCommandRequest& request,
            const RemoteCommandCallbackFn& cb,
            const BatonHandle& baton = nullptr) override {
            // Any error status other than ShutdownInProgress will cause the reporter to fassert.
            return Status(ErrorCodes::ShutdownInProgress,
                          "failed to send remote command - shutdown in progress");
        }
    };

    auto badExecutor =
        std::make_shared<TaskExecutorWithFailureInScheduleRemoteCommand>(&getExecutor());
    _executorProxy->setExecutor(badExecutor.get());

    ASSERT_OK(reporter->trigger());

    // Run callback to prepare command and attempt to send command to sync source.
    runReadyScheduledTasks();

    _executorProxy->setExecutor(&getExecutor());

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, reporter->join());
    ASSERT_FALSE(reporter->isActive());
}

TEST_F(ReporterTest, FailingToScheduleTimeoutShouldMakeReporterInactive) {
    class TaskExecutorWithFailureInScheduleWorkAt : public unittest::TaskExecutorProxy {
    public:
        using unittest::TaskExecutorProxy::TaskExecutorProxy;

        StatusWith<executor::TaskExecutor::CallbackHandle> scheduleWorkAt(Date_t when,
                                                                          CallbackFn&&) override {
            return Status(ErrorCodes::OperationFailed, "failed to schedule work");
        }
    };

    auto badExecutor = std::make_shared<TaskExecutorWithFailureInScheduleWorkAt>(&getExecutor());
    _executorProxy->setExecutor(badExecutor.get());

    processNetworkResponse(BSON("ok" << 1));

    _executorProxy->setExecutor(&getExecutor());

    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, KeepAliveTimeoutFailingToScheduleRemoteCommandShouldMakeReporterInactive) {
    processNetworkResponse(BSON("ok" << 1));

    auto until = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(until, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    Status expectedStatus(ErrorCodes::UnknownError, "failed to prepare update command");
    prepareReplSetUpdatePositionCommandFn = [expectedStatus]() -> StatusWith<BSONObj> {
        return expectedStatus;
    };

    runUntil(until);

    ASSERT_EQUALS(expectedStatus, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest,
       TriggerBeforeKeepAliveTimeoutShouldCancelExistingTimeoutAndSendUpdateImmediately) {
    processNetworkResponse(BSON("ok" << 1));

    auto keepAliveTimeoutWhen = getExecutor().now() + reporter->getKeepAliveInterval();

    ASSERT_EQUALS(keepAliveTimeoutWhen, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    auto until = keepAliveTimeoutWhen - reporter->getKeepAliveInterval() / 2;
    runUntil(until);

    ASSERT_OK(reporter->trigger());

    // '_keepAliveTimeoutWhen' is reset by trigger() not by the canceled callback.
    ASSERT_EQUALS(Date_t(), reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    processNetworkResponse(BSON("ok" << 1));

    keepAliveTimeoutWhen = getExecutor().now() + reporter->getKeepAliveInterval();

    // A new keep alive timeout should be scheduled.
    ASSERT_EQUALS(keepAliveTimeoutWhen, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    reporter->shutdown();
    runReadyNetworkOperations();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, ShutdownImmediatelyAfterTriggerWhileKeepAliveTimeoutIsScheduledShouldSucceed) {
    processNetworkResponse(BSON("ok" << 1));

    auto keepAliveTimeoutWhen = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(keepAliveTimeoutWhen, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    auto until = keepAliveTimeoutWhen - reporter->getKeepAliveInterval() / 2;
    runUntil(until);

    ASSERT_OK(reporter->trigger());

    // '_keepAliveTimeoutWhen' is reset by trigger() not by the canceled callback.
    ASSERT_EQUALS(Date_t(), reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    auto net = getNet();

    reporter->shutdown();

    net->enterNetwork();
    ASSERT_FALSE(net->hasReadyRequests());
    // Executor should invoke reporter callback with a ErrorCodes::CallbackCanceled status.
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, AllowUsingBackupChannel) {
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_OK(reporter->trigger());
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_OK(reporter->trigger(true /*allowOneMore*/));
    ASSERT_TRUE(reporter->isBackupActive());

    processNetworkResponse(BSON("ok" << 1), true);
    processNetworkResponse(BSON("ok" << 1), true);
    processNetworkResponse(BSON("ok" << 1), false);
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_TRUE(reporter->isActive());

    reporter->shutdown();
    runReadyNetworkOperations();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, BackupChannelFailureAlsoCauseReporterFailure) {
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_OK(reporter->trigger(true /*allowOneMore*/));
    ASSERT_TRUE(reporter->isBackupActive());

    processNetworkResponse(BSON("ok" << 1), true);
    processNetworkResponse(
        RemoteCommandResponse::make_forTest(Status(ErrorCodes::OperationFailed, "update failed"),
                                            Milliseconds(0)),
        false);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->getStatus_forTest().code());

    reporter->shutdown();
    runReadyNetworkOperations();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, BackupChannelSendAnotherOneAfterResponseIfReceiveTwo) {
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_OK(reporter->trigger(true /*allowOneMore*/));
    ASSERT_TRUE(reporter->isBackupActive());
    // Trigger on the backup channel one more time so reporter will send another request immediately
    // after one channel becomes free.
    ASSERT_OK(reporter->trigger(true /*allowOneMore*/));
    ASSERT_TRUE(reporter->isWaitingToSendReport());
    processNetworkResponse(BSON("ok" << 1), true);
    processNetworkResponse(BSON("ok" << 1), true);
    ASSERT_TRUE(reporter->isActive() || reporter->isBackupActive());
    ASSERT_FALSE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 1), false);

    reporter->shutdown();
    runReadyNetworkOperations();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTestNoTriggerAtSetUp, NotUsingBackupChannelWhenFree) {
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_FALSE(reporter->isActive());
    ASSERT_OK(reporter->trigger(true /*allowOneMore*/));
    ASSERT_FALSE(reporter->isBackupActive());
    ASSERT_TRUE(reporter->isActive());

    reporter->shutdown();
    runReadyNetworkOperations();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

}  // namespace
