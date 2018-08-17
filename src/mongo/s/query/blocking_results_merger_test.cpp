/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/query/blocking_results_merger.h"
#include "mongo/s/query/results_merger_test_fixture.h"

namespace mongo {

namespace {

using BlockingResultsMergerTest = ResultsMergerTestFixture;

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilKilled) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(
        operationContext(), makeARMParamsFromExistingCursors(std::move(cursors)), executor());

    blockingMerger.kill(operationContext());
}

TEST_F(ResultsMergerTestFixture, ShouldBeAbleToBlockUntilNextResultIsReady) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    BlockingResultsMerger blockingMerger(
        operationContext(), makeARMParamsFromExistingCursors(std::move(cursors)), executor());

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

TEST_F(ResultsMergerTestFixture, ShouldBeInterruptableDuringBlockingNext) {
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, 1, {})));
    auto params = makeARMParamsFromExistingCursors(std::move(cursors));
    BlockingResultsMerger blockingMerger(operationContext(), std::move(params), executor());

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

}  // namespace
}  // namespace mongo
