
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

#include "mongo/platform/basic.h"

#include "mongo/s/query/blocking_results_merger.h"
#include "mongo/s/query/results_merger_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using BlockingResultsMergerTest = ResultsMergerTestFixture;

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilKilled) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    blockingMerger.kill(operationContext());
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilDeadlineExpires) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto next = unittest::assertGet(
            blockingMerger.next(operationContext(), RouterExecStage::ExecContext::kInitialFind));

        // The timeout should hit, and return an empty object.
        ASSERT_TRUE(next.isEOF());
    });

    future.timed_get(kFutureTimeout);
    blockingMerger.kill(operationContext());
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilNextResultIsReady) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto next = unittest::assertGet(
            blockingMerger.next(operationContext(), RouterExecStage::ExecContext::kInitialFind));
        ASSERT_FALSE(next.isEOF());
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));
        next = unittest::assertGet(
            blockingMerger.next(operationContext(), RouterExecStage::ExecContext::kInitialFind));
        ASSERT_TRUE(next.isEOF());
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilNextResultIsReadyWithDeadline) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Used for synchronizing the background thread with this thread.
    stdx::mutex mutex;
    stdx::unique_lock<stdx::mutex> lk(mutex);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        // Will schedule a getMore. No one will send a response, so will return EOF.
        auto next = unittest::assertGet(blockingMerger.next(
            operationContext(), RouterExecStage::ExecContext::kGetMoreNoResultsYet));
        ASSERT_TRUE(next.isEOF());

        // Block until the main thread has responded to the getMore.
        stdx::unique_lock<stdx::mutex> lk(mutex);

        next = unittest::assertGet(blockingMerger.next(
            operationContext(), RouterExecStage::ExecContext::kGetMoreNoResultsYet));
        ASSERT_FALSE(next.isEOF());
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));

    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    // Unblock the other thread, allowing it to call next() on the BlockingResultsMerger.
    lk.unlock();

    future.timed_get(kFutureTimeout);
}

TEST_F(ResultsMergerTestFixture, ShouldBeInterruptableDuringBlockingNext) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = blockingMerger.next(operationContext(),
                                              RouterExecStage::ExecContext::kGetMoreNoResultsYet);
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Now mark the OperationContext as killed from this thread.
    {
        stdx::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }
    // Wait for the merger to be interrupted.
    future.timed_get(kFutureTimeout);

    // Now that we've seen it interrupted, kill it. We have to do this in another thread because
    // killing a BlockingResultsMerger involves running a killCursors, and this main thread is in
    // charge of scheduling the response to that request.
    future = launchAsync([&]() { blockingMerger.kill(operationContext()); });
    while (!networkHasReadyRequests() || !getNthPendingRequest(0u).cmdObj["killCursors"]) {
        // Wait for the kill to schedule it's killCursors. It may schedule a getMore first before
        // cancelling it, so wait until the pending request is actually a killCursors.
    }
    assertKillCusorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Run the callback for the killCursors. We don't actually inspect the value so we don't have to
    // schedule a response.
    runReadyCallbacks();
    future.timed_get(kFutureTimeout);
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToHandleExceptionWhenYielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) {
            uasserted(ErrorCodes::BadValue, "Simulated error");
        }

        void unyield(OperationContext*) {}
    };

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         std::make_unique<ThrowyResourceYielder>());

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        // Make sure that the next() call throws correctly.
        const auto status =
            blockingMerger.next(operationContext(), RouterExecStage::ExecContext::kInitialFind)
                .getStatus();
        ASSERT_EQ(status, ErrorCodes::BadValue);
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToHandleExceptionWhenUnyielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) {}

        void unyield(OperationContext*) {
            uasserted(ErrorCodes::BadValue, "Simulated error");
        }
    };

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         std::make_unique<ThrowyResourceYielder>());

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        // Make sure that the next() call throws correctly.
        const auto status =
            blockingMerger.next(operationContext(), RouterExecStage::ExecContext::kInitialFind)
                .getStatus();
        ASSERT_EQ(status, ErrorCodes::BadValue);
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
