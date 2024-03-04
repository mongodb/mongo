/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pinned_connection_task_executor_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/pinned_connection_task_executor.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo::executor {
namespace {

RemoteCommandRequest makeRCR(HostAndPort remote, BSONObj extraFields) {
    return RemoteCommandRequest(
        remote, DatabaseName::kAdmin, BSON("hello" << 1), extraFields, nullptr);
};

void assertMessageBodyCameFromRequest(Message m, RemoteCommandRequest rcr) {
    auto opMsg = OpMsgRequest::parse(m);
    auto expectedOpMsg = OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                                     std::move(rcr.dbname),
                                                     std::move(rcr.cmdObj),
                                                     std::move(rcr.metadata));
    ASSERT_BSONOBJ_EQ(opMsg.body, expectedOpMsg.body);
}

void assertMessageBodyAndDBName(Message m,
                                BSONObj body,
                                BSONObj metadata,
                                const DatabaseName& dbName) {
    auto opMsg = OpMsgRequest::parse(m);
    auto expectedOpMsg = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, dbName, body, metadata);
    ASSERT_BSONOBJ_EQ(opMsg.body, expectedOpMsg.body);
}

Message makeOkReplyMessage() {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(BSONObj());
    return replyBuilder.done();
}

Message makeErrorReplyMessage(Status error) {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(error);
    return replyBuilder.done();
}

TEST_F(PinnedConnectionTaskExecutorTest, RunSingleCommandOverSession) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    auto rcr = makeRCR(remote, BSONObj());
    auto pf = makePromiseFuture<void>();

    ASSERT_OK(pinnedTE->scheduleRemoteCommand(
        rcr, [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        }));
    // We first expect sink message to be called and to see the hello
    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyCameFromRequest(m, rcr);
        return Status::OK();
    });
    // Now we expect source message to be called and provide the response
    expectSourceMessage([&]() {
        auto message = makeOkReplyMessage();
        message.header().setResponseToMsgId(responseToId);
        return message;
    });

    ASSERT_OK(pf.future.getNoThrow());
    pinnedTE->shutdown();
    pinnedTE->join();
}

// Test we can schedule multiple RPC on the executor, and that they then
// run serially over the same transport session.
TEST_F(PinnedConnectionTaskExecutorTest, RunTwoRemoteCommandsSimultaneously) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    // Schedule two RPCs
    std::vector<Future<void>> results;
    for (int i = 0; i < 2; ++i) {
        auto promise = std::make_shared<Promise<void>>(NonNullPromiseTag{});
        results.push_back(promise->getFuture());
        auto extraFields = BSON("forTest" << i);
        ASSERT_OK(pinnedTE->scheduleRemoteCommand(
            makeRCR(remote, extraFields),
            [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                p->setWith([&] { return args.response.status; });
            }));
    }
    ASSERT_EQ(2, results.size());
    for (int i = 0; i < 2; ++i) {
        auto pf = makePromiseFuture<void>();
        // We first expect sink message to be called and to see the i'th request
        // (All i requests should appear on our same mocked session).
        int32_t responseToId;
        expectSinkMessage([&](Message m) {
            responseToId = m.header().getId();
            assertMessageBodyAndDBName(
                m, BSON("hello" << 1), BSON("forTest" << i), DatabaseName::kAdmin);
            pf.promise.emplaceValue();
            return Status::OK();
        });
        pf.future.get();
        // Now we expect source message to be called and provide the response
        expectSourceMessage([&]() {
            auto message = makeOkReplyMessage();
            message.header().setResponseToMsgId(responseToId);
            return message;
        });
        // I'th command should be completed:
        ASSERT_OK(results[i].getNoThrow());
    }
    pinnedTE->shutdown();
    pinnedTE->join();
}

TEST_F(PinnedConnectionTaskExecutorTest, FailCommandRemotelyDoesntBreakOtherCommands) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");
    //
    // Schedule two RPCs
    std::vector<Future<BSONObj>> results;
    for (int i = 0; i < 2; ++i) {
        auto promise = std::make_shared<Promise<BSONObj>>(NonNullPromiseTag{});
        results.push_back(promise->getFuture());
        auto extraFields = BSON("forTest" << i);
        ASSERT_OK(pinnedTE->scheduleRemoteCommand(
            makeRCR(remote, extraFields),
            [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                if (args.response.isOK()) {
                    p->emplaceValue(args.response.data);
                } else {
                    p->setError(args.response.status);
                }
            }));
    }
    ASSERT_EQ(2, results.size());

    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(
            m, BSON("hello" << 1), BSON("forTest" << 0), DatabaseName::kAdmin);
        return Status::OK();
    });
    // Fail the first request
    Status testFailure{ErrorCodes::BadValue, "test failure"};
    expectSourceMessage([&]() {
        auto message = makeErrorReplyMessage(testFailure);
        message.header().setResponseToMsgId(responseToId);
        return message;
    });
    auto remoteErr = results[0].getNoThrow().getValue();
    ASSERT_EQ(getStatusFromCommandResult(remoteErr), testFailure);

    // Second command should still be able to succeed:
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(
            m, BSON("hello" << 1), BSON("forTest" << 1), DatabaseName::kAdmin);
        return Status::OK();
    });
    expectSourceMessage([&]() {
        auto message = makeOkReplyMessage();
        message.header().setResponseToMsgId(responseToId);
        return message;
    });
    auto success = results[1].getNoThrow().getValue();
    ASSERT_EQ(Status::OK(), getStatusFromCommandResult(success));

    pinnedTE->shutdown();
    pinnedTE->join();
}

DEATH_TEST_REGEX_F(
    PinnedConnectionTaskExecutorTest,
    SchedulingCommandOnDifferentHostFails,
    R"#(Attempted to schedule RPC to (\S+):(\d+) on TaskExecutor that had pinned connection to (\S+):(\d+))#") {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");
    HostAndPort otherRemote("otherHost");

    // Schedule two RPCs
    auto pf = makePromiseFuture<void>();
    ASSERT_OK(pinnedTE->scheduleRemoteCommand(
        makeRCR(remote, {}), [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        }));
    auto pfTwo = makePromiseFuture<void>();
    ASSERT_OK(pinnedTE->scheduleRemoteCommand(
        makeRCR(otherRemote, {}), [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pfTwo.promise.setWith([&] { return args.response.status; });
        }));
    // first command runs OK
    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        return Status::OK();
    });
    expectSourceMessage([&]() {
        auto reply = makeOkReplyMessage();
        reply.header().setResponseToMsgId(responseToId);
        return reply;
    });
    ASSERT_OK(pf.future.getNoThrow());

    // Second command should invariant once the PCTE attempts to run it, because it has a different
    // remote target.
    // Should never be fulfilled.
    ASSERT_OK(pfTwo.future.getNoThrow());
}

TEST_F(PinnedConnectionTaskExecutorTest, CancelRPC) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    auto rcr = makeRCR(remote, BSONObj());
    auto pf = makePromiseFuture<void>();

    // Schedule a command.
    auto swCbHandle = pinnedTE->scheduleRemoteCommand(
        std::move(rcr), [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        });
    ASSERT_OK(swCbHandle);
    auto cbHandle = swCbHandle.getValue();
    pinnedTE->cancel(cbHandle);
    ASSERT_EQ(pf.future.getNoThrow(), TaskExecutor::kCallbackCanceledErrorStatus);

    pinnedTE->shutdown();
    pinnedTE->join();
}

TEST_F(PinnedConnectionTaskExecutorTest, ShutdownWithRPCInProgress) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    auto pf = makePromiseFuture<void>();
    ASSERT_OK(pinnedTE->scheduleRemoteCommand(
        makeRCR(HostAndPort("mock"), BSONObj()),
        [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        }));
    pinnedTE->shutdown();
    ASSERT_EQ(pf.future.getNoThrow(), TaskExecutor::kCallbackCanceledErrorStatus);
    pinnedTE->join();
}

TEST_F(PinnedConnectionTaskExecutorTest, CancelNonRPC) {
    auto pinnedTE = makePinnedConnTaskExecutor();

    auto pf = makePromiseFuture<void>();
    // Schedule some work
    auto now = getNet()->now();
    auto swCbHandle = pinnedTE->scheduleWorkAt(now + Milliseconds(10), [&](auto&& cbArgs) {
        pf.promise.setWith([&] { return cbArgs.status; });
    });

    ASSERT_OK(swCbHandle);
    auto cbHandle = swCbHandle.getValue();
    pinnedTE->cancel(cbHandle);

    ASSERT_EQ(pf.future.getNoThrow(), TaskExecutor::kCallbackCanceledErrorStatus);

    pinnedTE->shutdown();
    pinnedTE->join();
}

TEST_F(PinnedConnectionTaskExecutorTest, EnsureStreamIsUpdatedAfterUse) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    auto rcr = makeRCR(remote, BSONObj());
    auto pf = makePromiseFuture<void>();
    // We haven't done any RPCs, so we shouldn't have touched any of the stream counters.
    ASSERT_EQ(_indicateSuccessCalls.load(), 0);
    ASSERT_EQ(_indicateUsedCalls.load(), 0);
    ASSERT_EQ(_indicateFailureCalls.load(), 0);

    ASSERT_OK(pinnedTE->scheduleRemoteCommand(
        rcr, [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
            pf.promise.setWith([&] { return args.response.status; });
        }));
    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyCameFromRequest(m, rcr);
        return Status::OK();
    });
    expectSourceMessage([&]() {
        auto message = makeOkReplyMessage();
        message.header().setResponseToMsgId(responseToId);
        return message;
    });

    ASSERT_OK(pf.future.getNoThrow());

    pinnedTE->shutdown();
    pinnedTE->join();

    // We have compelted an RPC successfully using the leased stream:
    ASSERT_EQ(_indicateSuccessCalls.load(), 1);
    ASSERT_EQ(_indicateUsedCalls.load(), 1);
    ASSERT_EQ(_indicateFailureCalls.load(), 0);
}

TEST_F(PinnedConnectionTaskExecutorTest, StreamFailureShutsDownAndCancels) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    // We haven't done any RPCs, so we shouldn't have touched any of the stream counters.
    ASSERT_EQ(_indicateSuccessCalls.load(), 0);
    ASSERT_EQ(_indicateUsedCalls.load(), 0);
    ASSERT_EQ(_indicateFailureCalls.load(), 0);


    // Schedule two RPCs
    std::vector<Future<BSONObj>> results;
    for (int i = 0; i < 2; ++i) {
        auto promise = std::make_shared<Promise<BSONObj>>(NonNullPromiseTag{});
        results.push_back(promise->getFuture());
        auto extraFields = BSON("forTest" << i);
        ASSERT_OK(pinnedTE->scheduleRemoteCommand(
            makeRCR(remote, extraFields),
            [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                if (args.response.isOK()) {
                    p->emplaceValue(args.response.data);
                } else {
                    p->setError(args.response.status);
                }
            }));
    }
    ASSERT_EQ(2, results.size());

    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(
            m, BSON("hello" << 1), BSON("forTest" << 0), DatabaseName::kAdmin);
        return Status::OK();
    });

    // Fail the first request
    Status testFailure{ErrorCodes::BadValue, "test failure"};
    expectSourceMessage([&]() { return testFailure; });
    auto localErr = results[0].getNoThrow().getStatus();
    ASSERT_EQ(localErr, testFailure);

    // The second should be cancelled automatically by shutdown.
    ASSERT_EQ(results[1].getNoThrow(), TaskExecutor::kCallbackCanceledErrorStatus);
    ASSERT(pinnedTE->isShuttingDown());

    // We failed.
    ASSERT_EQ(_indicateSuccessCalls.load(), 0);
    ASSERT_EQ(_indicateUsedCalls.load(), 0);
    ASSERT_EQ(_indicateFailureCalls.load(), 1);
    pinnedTE->join();
}

/**
 * We want to test the following sequence:
 *  (1) A command is scheduled.
 *  (2) The command fails due to a network error.
 *  (3) The command is notified of the failure (its onResponse callback is invoked).
 *
 *  We want to ensure that the stream used by PCTE is destroyed _before_ the command is
 *  notified of the failure. This allows the underlying NetworkInterface to
 *  observe the failure on the initial stream & correctly update internally before it might
 *  be asked to provide another stream to i.e. retry the command.
 */
TEST_F(PinnedConnectionTaskExecutorTest, EnsureStreamDestroyedBeforeCommandCompleted) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    HostAndPort remote("mock");

    auto rcr = makeRCR(remote, BSON("forTest" << 0));
    auto pf = makePromiseFuture<void>();
    ASSERT_EQ(_streamDestroyedCalls.load(), 0);
    unittest::ThreadAssertionMonitor monitor;
    auto completionCallback = [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
        monitor.exec([&]() {
            // Ensure the stream was destroyed before we are notified of the command completing.
            ASSERT_EQ(_streamDestroyedCalls.load(), 1);
            pf.promise.setWith([&] { return args.response.status; });
            monitor.notifyDone();
        });
    };

    ASSERT_OK(pinnedTE->scheduleRemoteCommand(rcr, std::move(completionCallback)));

    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(
            m, BSON("hello" << 1), BSON("forTest" << 0), DatabaseName::kAdmin);
        return Status::OK();
    });

    // Fail the first request
    Status testFailure{ErrorCodes::BadValue, "test failure"};
    expectSourceMessage([&]() { return testFailure; });

    // Ensure we ran the completion callback.
    monitor.wait();

    auto localErr = pf.future.getNoThrow();
    ASSERT_EQ(localErr, testFailure);
}

}  // namespace
}  // namespace mongo::executor
