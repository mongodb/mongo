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

#include "mongo/db/repl/old_update_position_args.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
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
        _progressMap[memberId] = ProgressInfo(lastDurableOpTime, lastAppliedOpTime);
    }

    void clear() {
        _progressMap.clear();
    }

    long long getConfigVersion() const {
        return _configVersion;
    }

    void setConfigVersion(long long configVersion) {
        _configVersion = configVersion;
    }

    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle) {
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append(UpdatePositionArgs::kCommandFieldName, 1);
        BSONArrayBuilder arrayBuilder(
            cmdBuilder.subarrayStart(UpdatePositionArgs::kUpdateArrayFieldName));
        for (auto&& itr : _progressMap) {
            BSONObjBuilder entry(arrayBuilder.subobjStart());
            switch (commandStyle) {
                case ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kNewStyle:
                    itr.second.lastDurableOpTime.append(
                        &entry, UpdatePositionArgs::kDurableOpTimeFieldName);
                    itr.second.lastAppliedOpTime.append(
                        &entry, UpdatePositionArgs::kAppliedOpTimeFieldName);
                    break;
                case ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kOldStyle:
                    // Assume protocol version 1.
                    itr.second.lastDurableOpTime.append(&entry,
                                                        OldUpdatePositionArgs::kOpTimeFieldName);
                    break;
            }
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

        // Our last known OpTime that this slave has applied and journaled to.
        OpTime lastDurableOpTime;
        // Our last known OpTime that this slave has applied, whether journaled or unjournaled.
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
    BSONObj processNetworkResponse(ErrorCodes::Error code,
                                   const std::string& reason,
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
    virtual bool triggerAtSetUp() const override;
};

ReporterTest::ReporterTest() {}

void ReporterTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    _executorProxy = stdx::make_unique<unittest::TaskExecutorProxy>(&getExecutor());

    posUpdater = stdx::make_unique<MockProgressManager>();
    posUpdater->updateMap(0, OpTime({3, 0}, 1), OpTime({3, 0}, 1));

    prepareReplSetUpdatePositionCommandFn =
        stdx::bind(&MockProgressManager::prepareReplSetUpdatePositionCommand,
                   posUpdater.get(),
                   stdx::placeholders::_1);

    reporter = stdx::make_unique<Reporter>(
        _executorProxy.get(),
        [this](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle) {
            return prepareReplSetUpdatePositionCommandFn(commandStyle);
        },
        HostAndPort("h1"),
        Milliseconds(1000));
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
    executor::ThreadPoolExecutorTest::tearDown();
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

BSONObj ReporterTest::processNetworkResponse(ErrorCodes::Error code,
                                             const std::string& reason,
                                             bool expectReadyRequestsAfterProcessing) {
    auto net = getNet();
    net->enterNetwork();
    auto cmdObj = net->scheduleErrorResponse({code, reason}).cmdObj;
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

TEST_F(ReporterTestNoTriggerAtSetUp, InvalidConstruction) {
    // null PrepareReplSetUpdatePositionCommandFn
    ASSERT_THROWS(Reporter(&getExecutor(),
                           Reporter::PrepareReplSetUpdatePositionCommandFn(),
                           HostAndPort("h1"),
                           Milliseconds(1000)),
                  UserException);

    // null TaskExecutor
    ASSERT_THROWS_WHAT(
        Reporter(
            nullptr, prepareReplSetUpdatePositionCommandFn, HostAndPort("h1"), Milliseconds(1000)),
        UserException,
        "null task executor");

    // null PrepareReplSetUpdatePositionCommandFn
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                Reporter::PrepareReplSetUpdatePositionCommandFn(),
                                HostAndPort("h1"),
                                Milliseconds(1000)),
                       UserException,
                       "null function to create replSetUpdatePosition command object");

    // empty HostAndPort
    ASSERT_THROWS_WHAT(Reporter(&getExecutor(),
                                prepareReplSetUpdatePositionCommandFn,
                                HostAndPort(),
                                Milliseconds(1000)),
                       UserException,
                       "target name cannot be empty");

    // zero keep alive interval.
    ASSERT_THROWS_WHAT(
        Reporter(
            &getExecutor(), prepareReplSetUpdatePositionCommandFn, HostAndPort("h1"), Seconds(-1)),
        UserException,
        "keep alive interval must be positive");

    // negative keep alive interval.
    ASSERT_THROWS_WHAT(
        Reporter(
            &getExecutor(), prepareReplSetUpdatePositionCommandFn, HostAndPort("h1"), Seconds(-1)),
        UserException,
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

// If an error is returned, it should be recorded in the Reporter and not run again.
TEST_F(ReporterTest, TaskExecutorAndNetworkErrorsStopTheReporter) {
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(ErrorCodes::NoSuchKey, "waaaah");

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, UnsuccessfulCommandResponseStopsTheReporter) {
    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::UnknownError) << "errmsg"
                                     << "unknown error"));

    ASSERT_EQUALS(Status(ErrorCodes::UnknownError, "unknown error"), reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTestNoTriggerAtSetUp,
       InvalidReplicaSetResponseToARequestWithoutConfigVersionStopsTheReporter) {
    posUpdater->setConfigVersion(-1);
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isActive());

    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::InvalidReplicaSetConfig)
                                     << "errmsg"
                                     << "newer config"
                                     << "configVersion"
                                     << 100));

    ASSERT_EQUALS(Status(ErrorCodes::InvalidReplicaSetConfig, "invalid config"), reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, InvalidReplicaSetResponseWithoutConfigVersionOnSyncTargetStopsTheReporter) {
    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::InvalidReplicaSetConfig)
                                     << "errmsg"
                                     << "invalid config"));

    ASSERT_EQUALS(Status(ErrorCodes::InvalidReplicaSetConfig, "invalid config"), reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, InvalidReplicaSetResponseWithSameConfigVersionOnSyncTargetStopsTheReporter) {
    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::InvalidReplicaSetConfig)
                                     << "errmsg"
                                     << "invalid config"
                                     << "configVersion"
                                     << posUpdater->getConfigVersion()));

    ASSERT_EQUALS(Status(ErrorCodes::InvalidReplicaSetConfig, "invalid config"), reporter->join());
    assertReporterDone();
}

TEST_F(
    ReporterTest,
    InvalidReplicaSetResponseWithNewerConfigVersionOnSyncTargetToAnNewCommandStyleRequestDoesNotStopTheReporter) {
    // Reporter should not retry update command on sync source immediately after seeing newer
    // configuration.
    ASSERT_OK(reporter->trigger());
    ASSERT_TRUE(reporter->isWaitingToSendReport());

    processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::InvalidReplicaSetConfig)
                                     << "errmsg"
                                     << "newer config"
                                     << "configVersion"
                                     << posUpdater->getConfigVersion() + 1));

    ASSERT_TRUE(reporter->isActive());
}

TEST_F(
    ReporterTest,
    InvalidReplicaSetResponseWithNewerConfigVersionOnSyncTargetToAnOldCommandStyleRequestDoesNotStopTheReporter) {
    auto expectedNewStyleCommandRequest = unittest::assertGet(prepareReplSetUpdatePositionCommandFn(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kNewStyle));

    auto commandRequest =
        processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::BadValue) << "errmsg"
                                         << "Unexpected field durableOpTime in UpdateInfoArgs"),
                               true);
    ASSERT_EQUALS(expectedNewStyleCommandRequest, commandRequest);

    // Update command object should match old style (pre-3.2.4).
    auto expectedOldStyleCommandRequest = unittest::assertGet(prepareReplSetUpdatePositionCommandFn(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kOldStyle));

    commandRequest = processNetworkResponse(
        BSON("ok" << 0 << "code" << int(ErrorCodes::InvalidReplicaSetConfig) << "errmsg"
                  << "newer config"
                  << "configVersion"
                  << posUpdater->getConfigVersion() + 1));
    ASSERT_EQUALS(expectedOldStyleCommandRequest, commandRequest);

    ASSERT_TRUE(reporter->isActive());
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
    prepareReplSetUpdatePositionCommandFn =
        [expectedStatus](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle)
        -> StatusWith<BSONObj> { return expectedStatus; };
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
    prepareReplSetUpdatePositionCommandFn =
        [expectedStatus](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle)
        -> StatusWith<BSONObj> { return expectedStatus; };

    processNetworkResponse(BSON("ok" << 1));

    ASSERT_EQUALS(expectedStatus, reporter->join());
    assertReporterDone();
}

// If a remote server (most likely running with version before 3.2.4) returns ErrorCodes::BadValue
// on a new style replSetUpdateCommand command object, we should regenerate the command with
// pre-3.2.4 style arguments and retry the remote command.
TEST_F(ReporterTest,
       BadValueErrorOnNewStyleCommandShouldCauseRescheduleImmediatelyWithOldStyleCommand) {
    runReadyScheduledTasks();

    auto expectedNewStyleCommandRequest = unittest::assertGet(prepareReplSetUpdatePositionCommandFn(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kNewStyle));

    auto commandRequest =
        processNetworkResponse(BSON("ok" << 0 << "code" << int(ErrorCodes::BadValue) << "errmsg"
                                         << "Unexpected field durableOpTime in UpdateInfoArgs"),
                               true);
    ASSERT_EQUALS(expectedNewStyleCommandRequest, commandRequest);

    auto expectedOldStyleCommandRequest = unittest::assertGet(prepareReplSetUpdatePositionCommandFn(
        ReplicationCoordinator::ReplSetUpdatePositionCommandStyle::kOldStyle));

    commandRequest = processNetworkResponse(BSON("ok" << 1));

    // Update command object should match old style (pre-3.2.2).
    ASSERT_NOT_EQUALS(expectedNewStyleCommandRequest, expectedOldStyleCommandRequest);
    ASSERT_EQUALS(expectedOldStyleCommandRequest, commandRequest);

    reporter->shutdown();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, FailedUpdateShouldNotRescheduleUpdate) {
    processNetworkResponse(ErrorCodes::OperationFailed, "update failed");

    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, SuccessfulUpdateShouldRescheduleUpdate) {
    processNetworkResponse(BSON("ok" << 1));

    auto until = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(until, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    runUntil(until, true);

    processNetworkResponse(ErrorCodes::OperationFailed, "update failed");

    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    assertReporterDone();
}

TEST_F(ReporterTest, ShutdownWhileKeepAliveTimeoutIsScheduledShouldMakeReporterInactive) {
    processNetworkResponse(BSON("ok" << 1));

    auto until = getExecutor().now() + reporter->getKeepAliveInterval();
    ASSERT_EQUALS(until, reporter->getKeepAliveTimeoutWhen_forTest());
    ASSERT_TRUE(reporter->isActive());

    reporter->shutdown();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    ASSERT_FALSE(reporter->isActive());

    runUntil(until);
}

TEST_F(ReporterTestNoTriggerAtSetUp,
       FailingToSchedulePrepareCommandTaskShouldMakeReporterInactive) {
    class TaskExecutorWithFailureInScheduleWork : public unittest::TaskExecutorProxy {
    public:
        TaskExecutorWithFailureInScheduleWork(executor::TaskExecutor* executor)
            : unittest::TaskExecutorProxy(executor) {}
        virtual StatusWith<executor::TaskExecutor::CallbackHandle> scheduleWork(
            const CallbackFn& work) override {
            return Status(ErrorCodes::OperationFailed, "failed to schedule work");
        }
    };

    TaskExecutorWithFailureInScheduleWork badExecutor(&getExecutor());
    _executorProxy->setExecutor(&badExecutor);

    auto status = reporter->trigger();

    _executorProxy->setExecutor(&getExecutor());

    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_EQUALS(ErrorCodes::OperationFailed, reporter->join());
    ASSERT_FALSE(reporter->isActive());
}

TEST_F(ReporterTestNoTriggerAtSetUp, FailingToScheduleRemoteCommandTaskShouldMakeReporterInactive) {
    class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
    public:
        TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor)
            : unittest::TaskExecutorProxy(executor) {}
        virtual StatusWith<executor::TaskExecutor::CallbackHandle> scheduleRemoteCommand(
            const executor::RemoteCommandRequest& request,
            const RemoteCommandCallbackFn& cb) override {
            // Any error status other than ShutdownInProgress will cause the reporter to fassert.
            return Status(ErrorCodes::ShutdownInProgress,
                          "failed to send remote command - shutdown in progress");
        }
    };

    TaskExecutorWithFailureInScheduleRemoteCommand badExecutor(&getExecutor());
    _executorProxy->setExecutor(&badExecutor);

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
        TaskExecutorWithFailureInScheduleWorkAt(executor::TaskExecutor* executor)
            : unittest::TaskExecutorProxy(executor) {}
        virtual StatusWith<executor::TaskExecutor::CallbackHandle> scheduleWorkAt(
            Date_t when, const CallbackFn& work) override {
            return Status(ErrorCodes::OperationFailed, "failed to schedule work");
        }
    };

    TaskExecutorWithFailureInScheduleWorkAt badExecutor(&getExecutor());
    _executorProxy->setExecutor(&badExecutor);

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
    prepareReplSetUpdatePositionCommandFn =
        [expectedStatus](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle commandStyle)
        -> StatusWith<BSONObj> { return expectedStatus; };

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
    net->enterNetwork();
    ASSERT_TRUE(net->hasReadyRequests());
    net->exitNetwork();

    reporter->shutdown();

    net->enterNetwork();
    ASSERT_FALSE(net->hasReadyRequests());
    // Executor should invoke reporter callback with a ErrorCodes::CallbackCanceled status.
    net->runReadyNetworkOperations();
    net->exitNetwork();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, reporter->join());
    assertReporterDone();
}

}  // namespace
