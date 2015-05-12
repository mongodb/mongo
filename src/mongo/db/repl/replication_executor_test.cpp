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

#include <map>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

namespace {

    bool operator==(const RemoteCommandRequest lhs,
                    const RemoteCommandRequest rhs) {
        return lhs.target == rhs.target &&
            lhs.dbname == rhs.dbname &&
            lhs.cmdObj == rhs.cmdObj;
    }

    bool operator!=(const RemoteCommandRequest lhs,
                    const RemoteCommandRequest rhs) {
        return !(lhs == rhs);
    }

    void setStatus(const ReplicationExecutor::CallbackData& cbData, Status* target) {
        *target = cbData.status;
    }

    void setStatusAndShutdown(const ReplicationExecutor::CallbackData& cbData,
                              Status* target) {
        setStatus(cbData, target);
        if (cbData.status != ErrorCodes::CallbackCanceled)
            cbData.executor->shutdown();
    }

    void setStatusAndTriggerEvent(const ReplicationExecutor::CallbackData& cbData,
                                  Status* outStatus,
                                  ReplicationExecutor::EventHandle event) {
        *outStatus = cbData.status;
        if (!cbData.status.isOK())
            return;
        cbData.executor->signalEvent(event);
    }

    void scheduleSetStatusAndShutdown(const ReplicationExecutor::CallbackData& cbData,
                                      Status* outStatus1,
                                      Status* outStatus2) {
        if (!cbData.status.isOK()) {
            *outStatus1 = cbData.status;
            return;
        }
        *outStatus1= cbData.executor->scheduleWork(stdx::bind(setStatusAndShutdown,
                                                            stdx::placeholders::_1,
                                                            outStatus2)).getStatus();
    }

    const int64_t prngSeed = 1;

    TEST_F(ReplicationExecutorTest, RunOne) {
        ReplicationExecutor& executor = getExecutor();
        Status status(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                     stdx::placeholders::_1,
                                                     &status)).getStatus());
        executor.run();
        ASSERT_OK(status);
    }

    TEST_F(ReplicationExecutorTest, Schedule1ButShutdown) {
        ReplicationExecutor& executor = getExecutor();
        Status status(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   &status)).getStatus());
        executor.shutdown();
        executor.run();
        ASSERT_EQUALS(status, ErrorCodes::CallbackCanceled);
    }

    TEST_F(ReplicationExecutorTest, Schedule2Cancel1) {
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        ReplicationExecutor::CallbackHandle cb = unittest::assertGet(
            executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                             stdx::placeholders::_1,
                                             &status1)));
        executor.cancel(cb);
        ASSERT_OK(executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   &status2)).getStatus());
        executor.run();
        ASSERT_EQUALS(status1, ErrorCodes::CallbackCanceled);
        ASSERT_OK(status2);
    }

    TEST_F(ReplicationExecutorTest, OneSchedulesAnother) {
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWork(stdx::bind(scheduleSetStatusAndShutdown,
                                                   stdx::placeholders::_1,
                                                   &status1,
                                                   &status2)).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT_OK(status2);
    }

    class EventChainAndWaitingTest {
        MONGO_DISALLOW_COPYING(EventChainAndWaitingTest);
    public:
        EventChainAndWaitingTest();
        void run();
    private:
        void onGo(const ReplicationExecutor::CallbackData& cbData);
        void onGoAfterTriggered(const ReplicationExecutor::CallbackData& cbData);

        NetworkInterfaceMock* net;
        ReplicationExecutor executor;
        boost::thread executorThread;
        const ReplicationExecutor::EventHandle goEvent;
        const ReplicationExecutor::EventHandle event2;
        const ReplicationExecutor::EventHandle event3;
        ReplicationExecutor::EventHandle triggerEvent;
        ReplicationExecutor::CallbackFn  triggered2;
        ReplicationExecutor::CallbackFn  triggered3;
        Status status1;
        Status status2;
        Status status3;
        Status status4;
        Status status5;
    };

    TEST(ReplicationExecutorTest, EventChainAndWaiting) {
        EventChainAndWaitingTest().run();
    }

    EventChainAndWaitingTest::EventChainAndWaitingTest() :
        net(new NetworkInterfaceMock),
        executor(net, prngSeed),
        executorThread(stdx::bind(&ReplicationExecutor::run, &executor)),
        goEvent(unittest::assertGet(executor.makeEvent())),
        event2(unittest::assertGet(executor.makeEvent())),
        event3(unittest::assertGet(executor.makeEvent())),
        status1(ErrorCodes::InternalError, "Not mutated"),
        status2(ErrorCodes::InternalError, "Not mutated"),
        status3(ErrorCodes::InternalError, "Not mutated"),
        status4(ErrorCodes::InternalError, "Not mutated"),
        status5(ErrorCodes::InternalError, "Not mutated") {

        triggered2 = stdx::bind(setStatusAndTriggerEvent,
                                stdx::placeholders::_1,
                                &status2,
                                event2);
        triggered3 = stdx::bind(setStatusAndTriggerEvent,
                                stdx::placeholders::_1,
                                &status3,
                                event3);
    }

    void EventChainAndWaitingTest::run() {
        executor.onEvent(goEvent,
                         stdx::bind(&EventChainAndWaitingTest::onGo,
                                    this,
                                    stdx::placeholders::_1));
        executor.signalEvent(goEvent);
        executor.waitForEvent(goEvent);
        executor.waitForEvent(event2);
        executor.waitForEvent(event3);

        ReplicationExecutor::EventHandle neverSignaledEvent =
            unittest::assertGet(executor.makeEvent());
        boost::thread neverSignaledWaiter(stdx::bind(&ReplicationExecutor::waitForEvent,
                                                     &executor,
                                                     neverSignaledEvent));
        ReplicationExecutor::CallbackHandle shutdownCallback = unittest::assertGet(
                executor.scheduleWork(stdx::bind(setStatusAndShutdown,
                                                 stdx::placeholders::_1,
                                                 &status5)));
        executor.wait(shutdownCallback);
        neverSignaledWaiter.join();
        executorThread.join();
        ASSERT_OK(status1);
        ASSERT_OK(status2);
        ASSERT_OK(status3);
        ASSERT_OK(status4);
        ASSERT_OK(status5);
    }

    void EventChainAndWaitingTest::onGo(const ReplicationExecutor::CallbackData& cbData) {
        if (!cbData.status.isOK()) {
            status1 = cbData.status;
            return;
        }
        ReplicationExecutor* executor = cbData.executor;
        StatusWith<ReplicationExecutor::EventHandle> errorOrTriggerEvent = executor->makeEvent();
        if (!errorOrTriggerEvent.isOK()) {
            status1 = errorOrTriggerEvent.getStatus();
            executor->shutdown();
            return;
        }
        triggerEvent = errorOrTriggerEvent.getValue();
        StatusWith<ReplicationExecutor::CallbackHandle> cbHandle = executor->onEvent(
                triggerEvent, triggered2);
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }
        cbHandle = executor->onEvent(triggerEvent, triggered3);
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }

        cbHandle = executor->onEvent(
                goEvent,
                stdx::bind(&EventChainAndWaitingTest::onGoAfterTriggered,
                           this,
                           stdx::placeholders::_1));
        if (!cbHandle.isOK()) {
            status1 = cbHandle.getStatus();
            executor->shutdown();
            return;
        }
        status1 = Status::OK();
    }

    void EventChainAndWaitingTest::onGoAfterTriggered(
            const ReplicationExecutor::CallbackData& cbData) {
        status4 = cbData.status;
        if (!cbData.status.isOK()) {
            return;
        }
        cbData.executor->signalEvent(triggerEvent);
    }

    TEST_F(ReplicationExecutorTest, ScheduleWorkAt) {
        NetworkInterfaceMock* net = getNet();
        ReplicationExecutor& executor = getExecutor();
        launchExecutorThread();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        Status status2(ErrorCodes::InternalError, "Not mutated");
        Status status3(ErrorCodes::InternalError, "Not mutated");
        const Date_t now = net->now();
        const ReplicationExecutor::CallbackHandle cb1 =
            unittest::assertGet(executor.scheduleWorkAt(now + Milliseconds(100),
                                                        stdx::bind(setStatus,
                                                                   stdx::placeholders::_1,
                                                                   &status1)));
        unittest::assertGet(executor.scheduleWorkAt(now + Milliseconds(5000),
                                                    stdx::bind(setStatus,
                                                               stdx::placeholders::_1,
                                                               &status3)));
        const ReplicationExecutor::CallbackHandle cb2 =
            unittest::assertGet(executor.scheduleWorkAt(now + Milliseconds(200),
                                                        stdx::bind(setStatusAndShutdown,
                                                                   stdx::placeholders::_1,
                                                                   &status2)));
        const Date_t startTime = net->now();
        net->runUntil(startTime + Milliseconds(200));
        ASSERT_EQUALS(startTime + Milliseconds(200), net->now());
        executor.wait(cb1);
        executor.wait(cb2);
        ASSERT_OK(status1);
        ASSERT_OK(status2);
        executor.shutdown();
        joinExecutorThread();
        ASSERT_EQUALS(status3, ErrorCodes::CallbackCanceled);
    }

    std::string getRequestDescription(const RemoteCommandRequest& request) {
        return mongoutils::str::stream() << "Request(" << request.target.toString() << ", " <<
            request.dbname << ", " << request.cmdObj << ')';
    }

    static void setStatusOnRemoteCommandCompletion(
            const ReplicationExecutor::RemoteCommandCallbackData& cbData,
            const RemoteCommandRequest& expectedRequest,
            Status* outStatus) {

        if (cbData.request != expectedRequest) {
            *outStatus = Status(
                    ErrorCodes::BadValue,
                    mongoutils::str::stream() << "Actual request: " <<
                    getRequestDescription(cbData.request) << "; expected: " <<
                    getRequestDescription(expectedRequest));
            return;
        }
        *outStatus = cbData.response.getStatus();
    }

    TEST_F(ReplicationExecutorTest, ScheduleRemoteCommand) {
        NetworkInterfaceMock* net = getNet();
        ReplicationExecutor& executor = getExecutor();
        launchExecutorThread();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        const RemoteCommandRequest request(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ReplicationExecutor::CallbackHandle cbHandle = unittest::assertGet(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   request,
                                   &status1)));
        ASSERT(net->hasReadyRequests());
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        net->scheduleResponse(noi,
                              net->now(),
                              ResponseStatus(ErrorCodes::NoSuchKey, "I'm missing"));
        net->runReadyNetworkOperations();
        ASSERT(!net->hasReadyRequests());
        executor.wait(cbHandle);
        executor.shutdown();
        joinExecutorThread();
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, status1);
    }

    TEST_F(ReplicationExecutorTest, ScheduleAndCancelRemoteCommand) {
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        const RemoteCommandRequest request(
                HostAndPort("localhost", 27017),
                "mydb",
                BSON("whatsUp" << "doc"));
        ReplicationExecutor::CallbackHandle cbHandle = unittest::assertGet(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   request,
                                   &status1)));
        executor.cancel(cbHandle);
        launchExecutorThread();
        getNet()->runReadyNetworkOperations();
        executor.wait(cbHandle);
        executor.shutdown();
        joinExecutorThread();
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status1);
    }

    TEST_F(ReplicationExecutorTest, ScheduleDBWorkAndExclusiveWorkConcurrently) {
        boost::barrier barrier(2U);
        NamespaceString nss("mydb", "mycoll");
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        OperationContext* txn = nullptr;
        using CallbackData = ReplicationExecutor::CallbackData;
        ASSERT_OK(executor.scheduleDBWork([&](const CallbackData& cbData) {
            status1 = cbData.status;
            txn = cbData.txn;
            barrier.count_down_and_wait();
            if (cbData.status != ErrorCodes::CallbackCanceled) {
                cbData.executor->shutdown();
            }
        }).getStatus());
        ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
            barrier.count_down_and_wait();
        }).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT(txn);
    }

    TEST_F(ReplicationExecutorTest, ScheduleDBWorkWithCollectionLock) {
        NamespaceString nss("mydb", "mycoll");
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        OperationContext* txn = nullptr;
        bool collectionIsLocked = false;
        using CallbackData = ReplicationExecutor::CallbackData;
        ASSERT_OK(executor.scheduleDBWork([&](const CallbackData& cbData) {
            status1 = cbData.status;
            txn = cbData.txn;
            collectionIsLocked = txn ?
                txn->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X) :
                false;
            if (cbData.status != ErrorCodes::CallbackCanceled) {
                cbData.executor->shutdown();
            }
        }, nss, MODE_X).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT(txn);
        ASSERT_TRUE(collectionIsLocked);
    }

    TEST_F(ReplicationExecutorTest, ScheduleExclusiveLockOperation) {
        ReplicationExecutor& executor = getExecutor();
        Status status1(ErrorCodes::InternalError, "Not mutated");
        OperationContext* txn = nullptr;
        bool lockIsW = false;
        using CallbackData = ReplicationExecutor::CallbackData;
        ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
            status1 = cbData.status;
            txn = cbData.txn;
            lockIsW = txn ? txn->lockState()->isW() : false;
            if (cbData.status != ErrorCodes::CallbackCanceled) {
                cbData.executor->shutdown();
            }
        }).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT(txn);
        ASSERT_TRUE(lockIsW);
    }

    TEST_F(ReplicationExecutorTest, ShutdownBeforeRunningSecondExclusiveLockOperation) {
        ReplicationExecutor& executor = getExecutor();
        using CallbackData = ReplicationExecutor::CallbackData;
        Status status1(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
            status1 = cbData.status;
            if (cbData.status != ErrorCodes::CallbackCanceled) {
                cbData.executor->shutdown();
            }
        }).getStatus());
        // Second db work item is invoked by the main executor thread because the work item is
        // moved from the exclusive lock queue to the ready work item queue when the first callback
        // cancels the executor.
        Status status2(ErrorCodes::InternalError, "Not mutated");
        ASSERT_OK(executor.scheduleWorkWithGlobalExclusiveLock([&](const CallbackData& cbData) {
            status2 = cbData.status;
            if (cbData.status != ErrorCodes::CallbackCanceled) {
                cbData.executor->shutdown();
            }
        }).getStatus());
        executor.run();
        ASSERT_OK(status1);
        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status2.code());
    }

    TEST_F(ReplicationExecutorTest, RemoteCommandWithTimeout) {
        NetworkInterfaceMock* net = getNet();
        ReplicationExecutor& executor = getExecutor();
        Status status(ErrorCodes::InternalError, "");
        launchExecutorThread();
        const RemoteCommandRequest request(
                HostAndPort("lazy", 27017),
                "admin",
                BSON("sleep" << 1),
                Milliseconds(1));
        ReplicationExecutor::CallbackHandle cbHandle = unittest::assertGet(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   request,
                                   &status)));
        ASSERT(net->hasReadyRequests());
        const Date_t startTime = net->now();
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        net->scheduleResponse(noi,
                              startTime + Milliseconds(2),
                              ResponseStatus(ErrorCodes::ExceededTimeLimit, "I took too long"));
        net->runUntil(startTime + Milliseconds(2));
        ASSERT_EQUALS(startTime + Milliseconds(2), net->now());
        executor.wait(cbHandle);
        ASSERT_EQUALS(ErrorCodes::ExceededTimeLimit, status);
    }

    TEST_F(ReplicationExecutorTest, CallbackHandleComparison) {
        ReplicationExecutor& executor = getExecutor();
        Status status(ErrorCodes::InternalError, "");
        const RemoteCommandRequest request(
                HostAndPort("lazy", 27017),
                "admin",
                BSON("cmd" << 1));
        ReplicationExecutor::CallbackHandle cbHandle1 = unittest::assertGet(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   request,
                                   &status)));
        ReplicationExecutor::CallbackHandle cbHandle2 = unittest::assertGet(
                executor.scheduleRemoteCommand(
                        request,
                        stdx::bind(setStatusOnRemoteCommandCompletion,
                                   stdx::placeholders::_1,
                                   request,
                                   &status)));

        // test equality
        ASSERT_TRUE(cbHandle1 == cbHandle1);
        ASSERT_TRUE(cbHandle2 == cbHandle2);
        ASSERT_FALSE(cbHandle1 != cbHandle1);
        ASSERT_FALSE(cbHandle2 != cbHandle2);

        // test inequality
        ASSERT_TRUE(cbHandle1 != cbHandle2);
        ASSERT_TRUE(cbHandle2 != cbHandle1);
        ASSERT_FALSE(cbHandle1 == cbHandle2);
        ASSERT_FALSE(cbHandle2 == cbHandle1);

        ReplicationExecutor::CallbackHandle cbHandle1Copy = cbHandle1;
        ASSERT_TRUE(cbHandle1 == cbHandle1Copy);
        ASSERT_TRUE(cbHandle1Copy == cbHandle1);
        ASSERT_FALSE(cbHandle1Copy != cbHandle1);
        ASSERT_FALSE(cbHandle1 != cbHandle1Copy);

        std::vector<ReplicationExecutor::CallbackHandle> cbs;
        cbs.push_back(cbHandle1);
        cbs.push_back(cbHandle2);
        ASSERT(cbHandle1 != cbHandle2);
        std::vector<ReplicationExecutor::CallbackHandle>::iterator foundHandle =
                                                          std::find(cbs.begin(),
                                                                    cbs.end(),
                                                                    cbHandle1);
        ASSERT_TRUE(cbs.end() != foundHandle);
        ASSERT_TRUE(cbHandle1 == *foundHandle);
        launchExecutorThread();
        executor.shutdown();
        joinExecutorThread();
    }
}  // namespace
}  // namespace repl
}  // namespace mongo
