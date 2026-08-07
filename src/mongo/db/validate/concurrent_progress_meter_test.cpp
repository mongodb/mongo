// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/concurrent_progress_meter.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/progress_meter.h"

#include <mutex>

namespace {

using namespace mongo;

class ConcurrentProgressMeterHolderTest : public ServiceContextTest {
protected:
    static constexpr auto progressMeterLogText = "progress meter";
    static constexpr int secondsBetween = 0;
    static constexpr int checkInterval = 1;
};

// Four worker threads each contribute one hit concurrently against a threshold of 4. Because
// addAndFetch is sequentially consistent, the four threads produce return values {1,2,3,4} in
// some order; exactly the thread whose result equals 4 calls _flush(), draining all four hits to
// the underlying ProgressMeter in a single pm.hit(4) call. With secondsBetween=0 and
// checkInterval=1, that one call always emits the "progress meter" log line.
TEST_F(ConcurrentProgressMeterHolderTest, FlushesAtThreshold) {
    static constexpr int flushThreshold = 4;

    ProgressMeter pm(1000, secondsBetween, checkInterval);
    auto opCtxHolder = makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    ConcurrentProgressMeterHolder holder;
    {
        std::unique_lock<Client> lk(*opCtx->getClient());
        holder.set(lk, pm, opCtx, flushThreshold);
    }

    unittest::Barrier barrier(flushThreshold);
    unittest::LogCaptureGuard logs;

    {
        unittest::JoinThread t1([&] {
            barrier.countDownAndWait();
            holder.hit(1);
        });
        unittest::JoinThread t2([&] {
            barrier.countDownAndWait();
            holder.hit(1);
        });
        unittest::JoinThread t3([&] {
            barrier.countDownAndWait();
            holder.hit(1);
        });
        unittest::JoinThread t4([&] {
            barrier.countDownAndWait();
            holder.hit(1);
        });
    }

    ASSERT_EQ(logs.countTextContaining(progressMeterLogText), 1);
}

// Four worker threads each contribute two hits concurrently against a threshold of 100 — well
// above the total of 8 hits — so no flush fires mid-traversal. finished() then drains all eight
// escrowed hits in a single pm.hit(8) call, emitting exactly one log line.
TEST_F(ConcurrentProgressMeterHolderTest, FinishedFlushesRemainder) {
    static constexpr int64_t flushThreshold = 100;

    ProgressMeter pm(1000, secondsBetween, checkInterval);
    auto opCtxHolder = makeOperationContext();
    auto* opCtx = opCtxHolder.get();

    unittest::Barrier barrier(4);
    unittest::LogCaptureGuard logs;
    {
        ConcurrentProgressMeterHolder holder;
        {
            std::unique_lock<Client> lk(*opCtx->getClient());
            holder.set(lk, pm, opCtx, flushThreshold);
        }

        {
            unittest::JoinThread t1([&] {
                barrier.countDownAndWait();
                holder.hit(2);
            });
            unittest::JoinThread t2([&] {
                barrier.countDownAndWait();
                holder.hit(2);
            });
            unittest::JoinThread t3([&] {
                barrier.countDownAndWait();
                holder.hit(2);
            });
            unittest::JoinThread t4([&] {
                barrier.countDownAndWait();
                holder.hit(2);
            });
        }

        ASSERT_EQ(logs.countTextContaining(progressMeterLogText), 0);
    }
    ASSERT_EQ(logs.countTextContaining(progressMeterLogText), 1);
}

}  // namespace
