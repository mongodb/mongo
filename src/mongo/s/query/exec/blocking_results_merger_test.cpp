// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/blocking_results_merger.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/query/exec/results_merger_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

using BlockingResultsMergerTest = ResultsMergerTestFixture;

// Builds a getMore response object carrying a command error with the given error labels. Mirrors
// the helper of the same name in 'async_results_merger_test.cpp'. When 'baseBackoffMS' is supplied,
// it is attached as a 'baseBackoffMS' field so that the retry strategy uses it as the backoff base.
BSONObj makeResponseObjWithErrorLabels(int errorCode,
                                       std::string_view reason,
                                       std::vector<std::string_view> errorLabels,
                                       boost::optional<Milliseconds> baseBackoffMS = boost::none) {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("ok", 0);
    responseBuilder.append("code", errorCode);
    responseBuilder.append("errmsg", reason);

    BSONArrayBuilder arr(responseBuilder.subarrayStart("errorLabels"));
    for (const auto& label : errorLabels) {
        arr.append(label);
    }
    arr.done();

    if (baseBackoffMS) {
        responseBuilder.append("baseBackoffMS", static_cast<long long>(baseBackoffMS->count()));
    }

    return responseBuilder.obj();
}

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
    // Set the deadline to be two seconds in the future. We always test that the deadline
    // expires, so there's no racing.
    awaitDataState(operationContext()).waitForInsertsDeadline =
        getMockClockSource()->now() + Milliseconds{2000};
    awaitDataState(operationContext()).shouldWaitForInserts = true;

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        // Pass kGetMoreNoResultsYet so that the BRM will block and not just
        // return an empty batch immediately.
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));

        // The timeout should hit, and return EOF.
        ASSERT_TRUE(next.isEOF());
    });

    // Wait for a bit. Hopefully the other thread will be waiting for the clock to advance.
    // If not, we just advance the clock now, and when the other thread gets to that point
    // it will see that "now" has passed the deadline.
    sleepsecs(1);

    getMockClockSource()->advance(Milliseconds{3000});

    future.default_timed_get();

    // Answer the getMore, so that there are no more outstanding requests.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSONObj()})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });
    blockingMerger.kill(operationContext());
}


TEST_F(ResultsMergerTestFixture, ShouldBeAbleToRetrieveResultAfterHittingTimeoutOnceAndDetaching) {
    // Set the deadline to be two seconds in the future. We always test that the deadline
    // expires, so there's no racing.
    awaitDataState(operationContext()).waitForInsertsDeadline =
        getMockClockSource()->now() + Milliseconds{2000};
    awaitDataState(operationContext()).shouldWaitForInserts = true;

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        // Pass kGetMoreNoResultsYet so that the BRM will block and not just
        // return an empty batch immediately.
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));

        // The timeout should hit, and return EOF.
        ASSERT_TRUE(next.isEOF());
    });

    // Wait for a bit. Hopefully the other thread will be waiting for the clock to advance.
    // If not, we just advance the clock now, and when the other thread gets to that point
    // it will see that "now" has passed the deadline.
    sleepsecs(1);

    getMockClockSource()->advance(Milliseconds{3000});

    future.default_timed_get();
    ASSERT_FALSE(blockingMerger.remotesExhausted());

    // Detach and reattach.
    blockingMerger.detachFromOperationContext();

    blockingMerger.reattachToOperationContext(operationContext());

    awaitDataState(operationContext()).waitForInsertsDeadline =
        getMockClockSource()->now() + Milliseconds{2000};
    awaitDataState(operationContext()).shouldWaitForInserts = true;

    // Answer the getMore, so that there are no more outstanding requests.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("foo" << "bar"), BSON("bar" << "baz")})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    // Issue a blocking wait for the next results asynchronously on a different thread.
    future = launchAsync([&]() {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));

        // We should get back a result immediately now.
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("foo" << "bar"));

        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("bar" << "baz"));

        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    future.default_timed_get();

    // All results are consumed now.
    ASSERT_TRUE(blockingMerger.remotesExhausted());

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
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_FALSE(next.isEOF());
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));
        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilNextResultIsReadyWithDeadline) {
    // Set the deadline to be two seconds in the future. We always test that the deadline
    // expires, so there's no racing.
    awaitDataState(operationContext()).waitForInsertsDeadline =
        operationContext()->getServiceContext()->getPreciseClockSource()->now() +
        Milliseconds{2000};
    awaitDataState(operationContext()).shouldWaitForInserts = true;

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    params.setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Will schedule a getMore. No one will send a response in time, so will return EOF.
    auto future = launchAsync([&]() {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // Wait for a bit. Hopefully the other thread will be waiting for the clock to advance.
    // If not, we just advance the clock now, and when the other thread gets to that point
    // it will see that "now" has passed the deadline.
    sleepsecs(1);
    getMockClockSource()->advance(Milliseconds{3000});

    future.default_timed_get();

    // Used for synchronizing the background thread with this thread.
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);

    auto readyEvent = unittest::assertGet(blockingMerger.getNextEvent_forTest());

    // Issue a blocking wait for the next result asynchronously on a different thread.
    future = launchAsync([&]() {
        // Block until the main thread has responded to the getMore.
        std::unique_lock<std::mutex> lk(mutex);

        ASSERT_TRUE(executor()->waitForEvent(operationContext(), readyEvent).isOK());

        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
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

    future.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, ShouldBeInterruptibleDuringBlockingNext) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = blockingMerger.next(operationContext());
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Sleep to allow the blockingMerger.next() thread to started.  If the thread is not started
    // even after the sleep, then it's the same test as ShouldBeInterruptibleBeforeBlockingNext.
    sleepmillis(10);

    // Now mark the OperationContext as killed from this thread.
    {
        std::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }
    // Wait for the merger to be interrupted.
    future.default_timed_get();

    // Now that we've seen it interrupted, kill it. We have to do this in another thread because
    // killing a BlockingResultsMerger involves running a killCursors, and this main thread is in
    // charge of scheduling the response to that request.
    future = launchAsync([&]() { blockingMerger.kill(operationContext()); });
    while (!networkHasReadyRequests() || !getNthPendingRequest(0u).cmdObj["killCursors"]) {
        // Wait for the kill to schedule it's killCursors. It may schedule a getMore first before
        // cancelling it, so wait until the pending request is actually a killCursors.
    }
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    // Run the callback for the killCursors. We don't actually inspect the value so we don't have to
    // schedule a response.
    runReadyCallbacks();
    future.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, ShouldBeInterruptibleBeforeBlockingNext) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Mark the OperationContext as killed from this thread.
    {
        std::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }

    // Issue a blocking wait for the next result asynchronously on a different thread.
    auto future = launchAsync([&]() {
        auto nextStatus = blockingMerger.next(operationContext());
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Wait for the merger to be interrupted.
    future.default_timed_get();

    // Kill should complete.
    blockingMerger.kill(operationContext());
}

// Regression test for SERVER-123640. 'kill()' must complete even when called with an already-killed
// OperationContext. In production this scenario can cause an infinite hang: the old
// `_arm->kill(opCtx).wait()` used Interruptible::notInterruptible(), which does NOT drive the
// opCtx's baton. The ThreadPoolTaskExecutor delivers cancelled-getMore callbacks through the
// baton (via the ARM's sub-baton), so without the baton running the kill future was never
// signalled and 'wait()' blocked forever. The fix wraps the wait in
// 'runWithoutInterruptionExceptAtGlobalShutdown()' so that the opCtx is used as the Interruptible,
// which causes 'Waitable::wait()' to drive the baton and let the cancellation callbacks through.
TEST_F(ResultsMergerTestFixture, KillShouldCompleteWithInterruptedOpCtx) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking next() on a separate thread; this will schedule a getMore and block
    // waiting for a response, leaving an outstanding request in flight.
    auto nextFuture = launchAsync([&]() {
        auto nextStatus = blockingMerger.next(operationContext());
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::Interrupted);
    });

    // Give the background thread time to start blocking on the getMore response.
    sleepmillis(500);

    // Simulate a client disconnect / killOp by marking the OperationContext as killed.
    // next() will return Interrupted, but the getMore is still in flight in the executor.
    {
        std::lock_guard<Client> lk(*operationContext()->getClient());
        operationContext()->markKilled(ErrorCodes::Interrupted);
    }
    nextFuture.default_timed_get();

    // kill() is now called with the already-killed opCtx.  The ARM still has an outstanding
    // getMore request; kill() must wait for the kill future to be signalled (which happens when
    // the cancelled-getMore callback eventually runs).  Without the fix this could hang because
    // the old wait(notInterruptible) did not drive the baton that the executor uses to deliver
    // cancellation callbacks in production.
    auto killFuture = launchAsync([&]() { blockingMerger.kill(operationContext()); });

    // In the mock-network environment, cancellation callbacks are delivered when the network
    // processes its queue.  Wait until the ARM has scheduled the killCursors command (which
    // proves it has progressed past the cancel step), then flush all pending callbacks so the
    // kill future gets signalled and kill() can return.
    while (!networkHasReadyRequests() || !getNthPendingRequest(0u).cmdObj["killCursors"]) {
        // Spin until the ARM has issued the killCursors for the open cursor.
    }
    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    runReadyCallbacks();

    // kill() must complete without hanging.
    killFuture.default_timed_get();
}

// Regression test for SERVER-125621: without the fix, shutdown exceptions could escape from
// 'BlockingResultsMerger::kill()'.
TEST_F(ResultsMergerTestFixture, KillDoesNotLeakExceptions) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    // Issue a blocking next() on a separate thread; this will schedule a getMore and block waiting
    // for a response, leaving an outstanding request in flight.
    auto nextFuture = launchAsync([&]() {
        auto nextStatus = blockingMerger.next(operationContext());
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::InterruptedAtShutdown);
    });

    // Give the background thread time to start blocking on the getMore response.
    sleepmillis(500);

    // Simulate a shutdown.
    operationContext()->getServiceContext()->setKillAllOperations();

    nextFuture.default_timed_get();

    // kill() is now called while we are already in shutdown. The ARM still has an outstanding
    // getMore request; kill() must wait for the kill future to be signalled (which happens when
    // the cancelled-getMore callback eventually runs). Without the fix this could hang because
    // the old wait(notInterruptible) did not drive the baton that the executor uses to deliver
    // cancellation callbacks in production.
    auto killFuture = launchAsync([&]() { blockingMerger.kill(operationContext()); });

    // In the mock-network environment, cancellation callbacks are delivered when the network
    // processes its queue. Wait until the ARM has scheduled the killCursors command (which proves
    // it has progressed past the cancel step), then flush all pending callbacks so the kill future
    // gets signalled and kill() can return.
    while (!networkHasReadyRequests() || !getNthPendingRequest(0u).cmdObj["killCursors"]) {
        // Spin until the ARM has issued the killCursors for the open cursor.
    }

    assertKillCursorsCmdHasCursorId(getNthPendingRequest(0u).cmdObj, 1);

    runReadyCallbacks();

    // kill() must complete without hanging or throwing.
    killFuture.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToHandleExceptionWhenYielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {
            uasserted(ErrorCodes::BadValue, "Simulated error");
        }

        void unyield(OperationContext*) override {}
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
        const auto status = blockingMerger.next(operationContext()).getStatus();
        ASSERT_EQ(status, ErrorCodes::BadValue);
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToHandleExceptionWhenUnyielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {}

        void unyield(OperationContext*) override {
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
        const auto status = blockingMerger.next(operationContext()).getStatus();
        ASSERT_EQ(status, ErrorCodes::BadValue);
    });

    // Schedule the response to the getMore which will return the next result and mark the cursor as
    // exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.default_timed_get();
}

TEST_F(ResultsMergerTestFixture, CanAccessAsyncResultsMergerParams) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(
        operationContext(), std::move(params), executor(), nullptr);

    ASSERT_EQ(kTestNss, blockingMerger.asyncResultsMergerParams().getNss());
    ASSERT_EQ(1, blockingMerger.asyncResultsMergerParams().getRemotes().size());

    // Kill merger because otherwise it will run into an assertion in its dtor.
    blockingMerger.kill(operationContext());
}

//
// The following tests exercise the BlockingResultsMerger's automatic retry-with-backoff behavior on
// retryable getMore errors. The BlockingResultsMerger drives the retry loop internally: its
// 'blockUntilNext()' helper repeatedly calls 'nextEvent()' / 'waitForEvent()' until the underlying
// AsyncResultsMerger becomes ready, so a retryable error is retried transparently before 'next()'
// returns. The 'RetryableError' label alone implies a zero-delay backoff, so the ARM re-dispatches
// the retry as soon as the deferral callback fires.
//

// Tests that a single retryable error is retried automatically and the BlockingResultsMerger
// ultimately returns the results from the successful retry.
TEST_F(ResultsMergerTestFixture, BlockingResultsMergerRetriesSingleErrorAndSucceeds) {
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", 3);

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable, "transient error", {ErrorLabel::kRetryableError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    auto future = launchAsync([&] {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));

        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // The initial getMore fails with a retryable error.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    scheduleNetworkResponseObjs({retryableError});

    // The BRM's blocking loop drives the zero-delay backoff and re-dispatches the retry getMore.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    std::vector<CursorResponse> success;
    success.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{BSON("x" << 1)});
    scheduleNetworkResponses(std::move(success));

    future.default_timed_get();
    blockingMerger.kill(operationContext());
}

// Tests that a persistent error that resolves within the backoff strategy's attempt budget is
// retried repeatedly and ultimately succeeds.
TEST_F(ResultsMergerTestFixture, BlockingResultsMergerRetriesPersistentErrorUntilItResolves) {
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", 3);

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable, "persistent transient error", {ErrorLabel::kRetryableError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    // Two failures, then a success: still within the three-attempt budget.
    const int kNumFailuresBeforeSuccess = 2;
    auto future = launchAsync([&] {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));

        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    for (int i = 0; i < kNumFailuresBeforeSuccess; ++i) {
        while (!networkHasReadyRequests()) {
            sleepmillis(1);
        }
        scheduleNetworkResponseObjs({retryableError});
    }

    // The next retry succeeds, still within the backoff strategy's attempt budget.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    std::vector<CursorResponse> success;
    success.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{BSON("x" << 1)});
    scheduleNetworkResponses(std::move(success));

    future.default_timed_get();
    blockingMerger.kill(operationContext());
}

// Tests that a persistent error that outlasts the backoff strategy's attempt budget is surfaced
// to the caller of 'next()' rather than being retried forever.
TEST_F(ResultsMergerTestFixture, BlockingResultsMergerSurfacesErrorWhenBackoffBudgetExhausted) {
    const int maxAttempts = 3;
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", maxAttempts);

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable, "persistent error", {ErrorLabel::kRetryableError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    auto future = launchAsync([&] {
        auto nextStatus = blockingMerger.next(operationContext());
        ASSERT(!nextStatus.isOK());
        ASSERT_EQ(nextStatus.getStatus(), ErrorCodes::HostUnreachable);
        ASSERT_STRING_CONTAINS(nextStatus.getStatus().reason(), "persistent error");
    });

    // The initial getMore plus 'maxAttempts' retries all fail. Once the budget is exhausted the
    // final error is surfaced to the caller rather than retried again.
    for (int i = 0; i < maxAttempts + 1; ++i) {
        while (!networkHasReadyRequests()) {
            sleepmillis(1);
        }
        scheduleNetworkResponseObjs({retryableError});
    }

    future.default_timed_get();

    // The cursor was not exhausted by a successful response, so it must be killed before the BRM is
    // destroyed.
    blockingMerger.kill(operationContext());
}

// Tests that an OperationContext can be detached and reattached while a delayed backoff retry
// is pending. This mirrors the production scenario where a mongos cursor is stashed (detached)
// between client getMores in the middle of the ARM's backoff retry loop. 'SystemOverloadedError'
// alongside 'RetryableError' triggers a non-zero backoff, giving the test a window to swap the
// OperationContext before the retry fires.
TEST_F(ResultsMergerTestFixture,
       BlockingResultsMergerRetriesAfterDetachingAndReattachingMidBackoff) {
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", 3);
    constexpr auto kBackoffDelayMs = 1000;
    FailPointEnableBlock fp{"setBackoffDelayForTesting", BSON("backoffDelayMs" << kBackoffDelayMs)};

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable,
        "transient error",
        {ErrorLabel::kRetryableError, ErrorLabel::kSystemOverloadedError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    auto future = launchAsync([&] {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));

        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // Wait for the initial getMore to be dispatched, then detach the OperationContext (simulating
    // the cursor being stashed between client getMores) before delivering the error response.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    blockingMerger.detachFromOperationContext();

    // Deliver the retryable error while detached. Because the ARM has no OperationContext, the
    // retry is scheduled via the executor's 'sleepFor' (a timer on the mock network) rather than
    // the SubBaton, so it will not fire until the network clock is advanced below.
    scheduleNetworkResponseObjs({retryableError});

    // Reattach with a fresh OperationContext. The pending retry must not be lost.
    blockingMerger.reattachToOperationContext(operationContext());

    // Fire the backoff timer and pump the network so the retry callback defers and signals the
    // event the BRM is blocked on. The BRM's loop then re-dispatches the retry getMore using the
    // reattached OperationContext.
    advanceTime(Milliseconds(kBackoffDelayMs + 1));
    runReadyCallbacks();

    // The retry getMore is now dispatched; answer it with a successful, exhausting batch.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    std::vector<CursorResponse> success;
    success.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{BSON("x" << 1)});
    scheduleNetworkResponses(std::move(success));

    future.default_timed_get();
    blockingMerger.kill(operationContext());
}

// The ARM schedules a backoff retry through one of two mechanisms depending on whether it is
// attached to an OperationContext:
//
//   * While attached, the retry is scheduled via
//     '_subBaton->waitUntil(preciseNow + delay, token).thenRunOn(_executor).getAsync(cb)'.
//     'SubBaton::waitUntil' forwards to the opCtx's parent DefaultBaton, which stores the timer in
//     its '_timers' map keyed on the *precise* clock source.
//   * While detached, the retry is scheduled via '_executor->sleepFor(delay, token).getAsync(cb)',
//     whose timer is driven by the mock *network* clock.
//
// This difference makes the testing harness strategy differ between the two, as you can see below.
//
// The next two tests pin the "not too soon" half of that contract for the BRM: the retry getMore
// must NOT be dispatched until the mock clock advances past the backoff deadline.

TEST_F(ResultsMergerTestFixture,
       BlockingResultsMergerDoesNotRetryBeforeBackoffDeadlineWhileAttached) {
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", 3);
    constexpr auto kBackoffDelayMs = 1000;
    FailPointEnableBlock fp{"setBackoffDelayForTesting", BSON("backoffDelayMs" << kBackoffDelayMs)};
    // Observation-only: lets us deterministically wait for the retry alarm to be armed before
    // advancing the mock clock, replacing a racy fixed 'sleepmillis'.
    FailPointEnableBlock retryScheduledFp{"armRetryScheduledForTesting"};

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable,
        "transient error",
        {ErrorLabel::kRetryableError, ErrorLabel::kSystemOverloadedError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    auto future = launchAsync([&] {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));
        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // The initial getMore fails with a retryable error that carries a non-zero backoff.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    scheduleNetworkResponseObjs({retryableError});

    // Deterministically wait for the error to be processed and the SubBaton timer armed before
    // advancing the mock clock: the deadline is then fixed at mockNow(0) + kBackoffDelayMs, so the
    // timer cannot have fired yet. Use a count relative to 'initialTimesEntered()' rather than an
    // absolute 1, because 'armRetryScheduledForTesting' is a global fail point whose entry counter
    // persists across tests in this binary and an absolute target would be satisfied immediately if
    // an earlier test already entered it. Driving the baton here also flushes the
    // timer-registration job that 'waitUntil' may have queued (rather than run inline) while the
    // BRM's blocked thread was sleeping in 'run_until'.
    retryScheduledFp->waitForTimesEntered(retryScheduledFp.initialTimesEntered() + 1);
    runScheduledTasks(operationContext());
    runReadyCallbacks();
    ASSERT_FALSE(networkHasReadyRequests());

    // Advancing the mock clock partway is still before the deadline: no retry yet. Drive the baton
    // so any timer due by now would fire; none is, so 'outstandingRequest' stays true and no
    // getMore is dispatched.
    advanceTime(Milliseconds(kBackoffDelayMs / 2));
    runScheduledTasks(operationContext());
    runReadyCallbacks();
    ASSERT_FALSE(networkHasReadyRequests());

    // Only once the mock clock passes the backoff deadline does the SubBaton timer fire. Driving
    // the baton fires it and posts the retry continuation onto the executor; pumping the executor
    // runs it, which signals the event the BRM is blocked on, so the BRM's loop re-dispatches the
    // retry getMore.
    advanceTime(Milliseconds(kBackoffDelayMs / 2 + 1));
    runScheduledTasks(operationContext());
    runReadyCallbacks();
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    std::vector<CursorResponse> success;
    success.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{BSON("x" << 1)});
    scheduleNetworkResponses(std::move(success));

    future.default_timed_get();
    blockingMerger.kill(operationContext());
}

// While detached, the retry getMore is not dispatched until the *network* clock advances past the
// backoff deadline (via 'advanceTime()'). This clarifies that the 'advanceTime' trick — not
// 'getMockClockSource()->advance' — is what drives a retry scheduled while detached, because the
// detached path uses '_executor->sleepFor' rather than the SubBaton.
TEST_F(ResultsMergerTestFixture,
       BlockingResultsMergerDoesNotRetryBeforeBackoffDeadlineWhileDetached) {
    unittest::ServerParameterGuard maxAttemptsGuard("defaultClientMaxRetryAttempts", 3);
    constexpr auto kBackoffDelayMs = 1000;
    FailPointEnableBlock fp{"setBackoffDelayForTesting", BSON("backoffDelayMs" << kBackoffDelayMs)};
    // Observation-only: lets us deterministically wait for the 'sleepFor' timer to be armed before
    // advancing the mock clock, replacing a racy fixed 'sleepmillis'.
    FailPointEnableBlock retryScheduledFp{"armRetryScheduledForTesting"};

    const BSONObj retryableError = makeResponseObjWithErrorLabels(
        ErrorCodes::HostUnreachable,
        "transient error",
        {ErrorLabel::kRetryableError, ErrorLabel::kSystemOverloadedError});

    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(operationContext(),
                                         makeARMParamsFromExistingCursors(std::move(cursors)),
                                         executor(),
                                         nullptr);

    auto future = launchAsync([&] {
        auto next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_BSONOBJ_EQ(*next.getResult(), BSON("x" << 1));
        next = unittest::assertGet(blockingMerger.next(operationContext()));
        ASSERT_TRUE(next.isEOF());
    });

    // Detach before delivering the error so the ARM schedules the retry via 'sleepFor' (network
    // clock) rather than the SubBaton (precise clock), then reattach.
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    blockingMerger.detachFromOperationContext();
    scheduleNetworkResponseObjs({retryableError});
    blockingMerger.reattachToOperationContext(operationContext());

    // Deterministically wait for the detached retry (scheduled via 'sleepFor' on the *network*
    // clock) to be armed. The deadline is fixed at networkClock(0) + kBackoffDelayMs before we
    // touch the clock below, so the negative assertion is meaningful. The count is relative to
    // 'initialTimesEntered()' for the same cross-test counter reason as the attached test above.
    retryScheduledFp->waitForTimesEntered(retryScheduledFp.initialTimesEntered() + 1);
    ASSERT_FALSE(networkHasReadyRequests());

    // Advancing the network clock partway is still before the deadline: no retry yet.
    advanceTime(Milliseconds(kBackoffDelayMs / 2));
    runReadyCallbacks();
    ASSERT_FALSE(networkHasReadyRequests());

    // Only once the network clock passes the backoff deadline does the 'sleepFor' timer fire, the
    // retry callback defer, and the BRM's loop re-dispatch the retry getMore.
    advanceTime(Milliseconds(kBackoffDelayMs / 2 + 1));
    runReadyCallbacks();
    while (!networkHasReadyRequests()) {
        sleepmillis(1);
    }
    std::vector<CursorResponse> success;
    success.emplace_back(kTestNss, CursorId(0), std::vector<BSONObj>{BSON("x" << 1)});
    scheduleNetworkResponses(std::move(success));

    future.default_timed_get();
    blockingMerger.kill(operationContext());
}

}  // namespace
}  // namespace mongo
