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

#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include "pinned_connection_task_executor_test_fixture.h"

namespace mongo::executor {
namespace {

RemoteCommandRequest makeRCR(HostAndPort remote, BSONObj extraFields) {
    return RemoteCommandRequest(remote, "admin", BSON("hello" << 1), extraFields, nullptr);
};

void assertMessageBodyCameFromRequest(Message m, RemoteCommandRequest rcr) {
    auto opMsg = OpMsgRequest::parse(m);
    auto expectedOpMsg = OpMsgRequest::fromDBAndBody(
        std::move(rcr.dbname), std::move(rcr.cmdObj), std::move(rcr.metadata));
    ASSERT_BSONOBJ_EQ(opMsg.body, expectedOpMsg.body);
}

void assertMessageBodyAndDBName(Message m, BSONObj body, BSONObj metadata, std::string dbName) {
    auto opMsg = OpMsgRequest::parse(m);
    auto expectedOpMsg = OpMsgRequest::fromDBAndBody(dbName, body, metadata);
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

    ASSERT_OK(pinnedTE
                  ->scheduleRemoteCommand(rcr,
                                          [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                              pf.promise.setWith(
                                                  [&] { return args.response.status; });
                                          })
                  .getStatus());
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
        ASSERT_OK(
            pinnedTE
                ->scheduleRemoteCommand(
                    makeRCR(remote, extraFields),
                    [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                        p->setWith([&] { return args.response.status; });
                    })
                .getStatus());
    }
    ASSERT_EQ(2, results.size());
    for (int i = 0; i < 2; ++i) {
        auto pf = makePromiseFuture<void>();
        // We first expect sink message to be called and to see the i'th request
        // (All i requests should appear on our same mocked session).
        int32_t responseToId;
        expectSinkMessage([&](Message m) {
            responseToId = m.header().getId();
            assertMessageBodyAndDBName(m, BSON("hello" << 1), BSON("forTest" << i), "admin");
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
        ASSERT_OK(
            pinnedTE
                ->scheduleRemoteCommand(
                    makeRCR(remote, extraFields),
                    [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                        if (args.response.isOK()) {
                            p->emplaceValue(args.response.data);
                        } else {
                            p->setError(args.response.status);
                        }
                    })
                .getStatus());
    }
    ASSERT_EQ(2, results.size());

    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(m, BSON("hello" << 1), BSON("forTest" << 0), "admin");
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
        assertMessageBodyAndDBName(m, BSON("hello" << 1), BSON("forTest" << 1), "admin");
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
    ASSERT_OK(pinnedTE
                  ->scheduleRemoteCommand(makeRCR(remote, {}),
                                          [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                              pf.promise.setWith(
                                                  [&] { return args.response.status; });
                                          })
                  .getStatus());
    auto pfTwo = makePromiseFuture<void>();
    ASSERT_OK(pinnedTE
                  ->scheduleRemoteCommand(makeRCR(otherRemote, {}),
                                          [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                              pfTwo.promise.setWith(
                                                  [&] { return args.response.status; });
                                          })
                  .getStatus());
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
    ASSERT_OK(swCbHandle.getStatus());
    auto cbHandle = swCbHandle.getValue();
    pinnedTE->cancel(cbHandle);
    ASSERT_EQ(pf.future.getNoThrow(), TaskExecutor::kCallbackCanceledErrorStatus);

    pinnedTE->shutdown();
    pinnedTE->join();
}

TEST_F(PinnedConnectionTaskExecutorTest, ShutdownWithRPCInProgress) {
    auto pinnedTE = makePinnedConnTaskExecutor();
    auto pf = makePromiseFuture<void>();
    ASSERT_OK(pinnedTE
                  ->scheduleRemoteCommand(makeRCR(HostAndPort("mock"), BSONObj()),
                                          [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                              pf.promise.setWith(
                                                  [&] { return args.response.status; });
                                          })
                  .getStatus());
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

    ASSERT_OK(swCbHandle.getStatus());
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

    ASSERT_OK(pinnedTE
                  ->scheduleRemoteCommand(rcr,
                                          [&](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                                              pf.promise.setWith(
                                                  [&] { return args.response.status; });
                                          })
                  .getStatus());
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
        ASSERT_OK(
            pinnedTE
                ->scheduleRemoteCommand(
                    makeRCR(remote, extraFields),
                    [p = std::move(promise)](const TaskExecutor::RemoteCommandCallbackArgs& args) {
                        if (args.response.isOK()) {
                            p->emplaceValue(args.response.data);
                        } else {
                            p->setError(args.response.status);
                        }
                    })
                .getStatus());
    }
    ASSERT_EQ(2, results.size());

    int32_t responseToId;
    expectSinkMessage([&](Message m) {
        responseToId = m.header().getId();
        assertMessageBodyAndDBName(m, BSON("hello" << 1), BSON("forTest" << 0), "admin");
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
}  // namespace
}  // namespace mongo::executor
