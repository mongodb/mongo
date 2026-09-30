// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/fail_point.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

using mongo::BSONObj;
using mongo::FailPoint;
using mongo::FailPointEnableBlock;

namespace stdx = mongo::stdx;

namespace mongo_test {
namespace {

#if 0  // Uncomment this block to manually test the _valid flag operation
extern FailPoint notYetFailPointTest;
[[maybe_unused]] bool expectAnInvariantViolation = notYetFailPointTest.shouldFail();
MONGO_FAIL_POINT_DEFINE(notYetFailPointTest);
#endif

// Used by tests in this file that need access to a failpoint that is a registered in the
// FailPointRegistry.
MONGO_FAIL_POINT_DEFINE(dummy2);
}  // namespace

TEST(FailPoint, InitialState) {
    FailPoint failPoint("testFP");
    ASSERT_FALSE(failPoint.shouldFail());
}

TEST(FailPoint, AlwaysOn) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);
    ASSERT(failPoint.shouldFail());

    if (auto scopedFp = failPoint.scoped(); MONGO_unlikely(scopedFp.isActive())) {
        ASSERT(scopedFp.getData().isEmpty());
    }

    for (size_t x = 0; x < 50; x++) {
        ASSERT(failPoint.shouldFail());
    }
}

TEST(FailPoint, NTimes) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::nTimes, 4);
    ASSERT(failPoint.shouldFail());
    ASSERT(failPoint.shouldFail());
    ASSERT(failPoint.shouldFail());
    ASSERT(failPoint.shouldFail());

    for (size_t x = 0; x < 50; x++) {
        ASSERT_FALSE(failPoint.shouldFail());
    }
}

TEST(FailPoint, BlockOff) {
    FailPoint failPoint("testFP");
    bool called = false;
    failPoint.execute([&](const BSONObj&) { called = true; });
    ASSERT_FALSE(called);
}

TEST(FailPoint, BlockAlwaysOn) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);
    bool called = false;

    failPoint.execute([&](const BSONObj&) { called = true; });

    ASSERT(called);
}

TEST(FailPoint, BlockNTimes) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::nTimes, 1);
    size_t counter = 0;

    for (size_t x = 0; x < 10; x++) {
        failPoint.execute([&](auto&&...) { counter++; });
    }

    ASSERT_EQUALS(1U, counter);
}

TEST(FailPoint, BlockWithException) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);
    bool threw = false;

    try {
        failPoint.execute(
            [&](const BSONObj&) { throw std::logic_error("BlockWithException threw"); });
    } catch (const std::logic_error&) {
        threw = true;
    }

    ASSERT(threw);
    // This will get into an infinite loop if reference counter was not
    // properly decremented
    failPoint.setMode(FailPoint::off);
}

TEST(FailPoint, SetGetParam) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("x" << 20));

    failPoint.execute([&](const BSONObj& data) { ASSERT_EQUALS(20, data["x"].numberInt()); });
}

TEST(FailPoint, DisableAllFailpoints) {
    auto& registry = mongo::globalFailPointRegistry();

    FailPoint& fp1 = *registry.find("dummy");
    FailPoint& fp2 = *registry.find("dummy2");
    int counter1 = 0;
    int counter2 = 0;
    fp1.execute([&](const BSONObj&) { counter1++; });
    fp2.execute([&](const BSONObj&) { counter2++; });

    ASSERT_EQ(0, counter1);
    ASSERT_EQ(0, counter2);

    fp1.setMode(FailPoint::alwaysOn);
    fp2.setMode(FailPoint::alwaysOn);

    fp1.execute([&](const BSONObj&) { counter1++; });
    fp2.execute([&](const BSONObj&) { counter2++; });

    ASSERT_EQ(1, counter1);
    ASSERT_EQ(1, counter2);

    registry.disableAllFailpoints();

    fp1.execute([&](const BSONObj&) { counter1++; });
    fp2.execute([&](const BSONObj&) { counter2++; });

    ASSERT_EQ(1, counter1);
    ASSERT_EQ(1, counter2);

    // Check that you can still enable and continue using FailPoints after a call to
    // disableAllFailpoints()
    fp1.setMode(FailPoint::alwaysOn);
    fp2.setMode(FailPoint::alwaysOn);

    fp1.execute([&](const BSONObj&) { counter1++; });
    fp2.execute([&](const BSONObj&) { counter2++; });

    ASSERT_EQ(2, counter1);
    ASSERT_EQ(2, counter2);

    // Reset the state for future tests.
    registry.disableAllFailpoints();
}

TEST(FailPoint, Stress) {
    mongo::unittest::ThreadAssertionMonitor monitor;
    monitor
        .spawnController([&] {
            mongo::Atomic<bool> done{false};
            FailPoint fp("testFP");
            fp.setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
            auto fpGuard =
                mongo::ScopeGuard([&] { fp.setMode(FailPoint::off, 0, BSON("a" << 66)); });
            std::vector<stdx::thread> tasks;
            mongo::ScopeGuard joinGuard = [&] {
                for (auto&& t : tasks)
                    if (t.joinable())
                        t.join();
            };
            auto launchLoop = [&](auto&& f) {
                tasks.push_back(monitor.spawn([&, f] {
                    while (!done.load())
                        f();
                }));
            };
            launchLoop([&] {
                fp.execute([](const BSONObj& data) {
                    ASSERT_EQ(data["a"].numberInt(), 44) << "blockTask" << data.toString();
                });
            });
            launchLoop([&] {
                try {
                    fp.execute([](const BSONObj& data) {
                        ASSERT_EQ(data["a"].numberInt(), 44)
                            << "blockWithExceptionTask" << data.toString();
                        throw std::logic_error("blockWithExceptionTask threw");
                    });
                } catch (const std::logic_error&) {
                }
            });
            launchLoop([&] { fp.shouldFail(); });
            launchLoop([&] {
                if (fp.shouldFail()) {
                    fp.setMode(FailPoint::off, 0);
                } else {
                    fp.setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
                }
            });
            mongo::sleepsecs(5);
            done.store(true);
        })
        .join();
}

static void parallelFailPointTestThread(FailPoint* fp,
                                        const int64_t numIterations,
                                        const int32_t seed,
                                        int64_t* outNumActivations) {
    fp->setThreadPRNGSeed(seed);
    int64_t numActivations = 0;
    for (int64_t i = 0; i < numIterations; ++i) {
        if (fp->shouldFail()) {
            ++numActivations;
        }
    }
    *outNumActivations = numActivations;
}
/**
 * Encounters a failpoint with the given fpMode and fpVal numEncountersPerThread
 * times in each of numThreads parallel threads, and returns the number of total
 * times that the failpoint was activiated.
 */
static int64_t runParallelFailPointTest(FailPoint::Mode fpMode,
                                        FailPoint::ValType fpVal,
                                        const int32_t numThreads,
                                        const int32_t numEncountersPerThread) {
    ASSERT_GT(numThreads, 0);
    ASSERT_GT(numEncountersPerThread, 0);
    FailPoint failPoint("testFP");
    failPoint.setMode(fpMode, fpVal);
    std::vector<stdx::thread*> tasks;
    std::vector<int64_t> counts(numThreads, 0);
    ASSERT_EQUALS(static_cast<uint32_t>(numThreads), counts.size());
    for (int32_t i = 0; i < numThreads; ++i) {
        tasks.push_back(new stdx::thread(parallelFailPointTestThread,
                                         &failPoint,
                                         numEncountersPerThread,
                                         i,  // hardcoded seed, different for each thread.
                                         &counts[i]));
    }
    int64_t totalActivations = 0;
    for (int32_t i = 0; i < numThreads; ++i) {
        tasks[i]->join();
        delete tasks[i];
        totalActivations += counts[i];
    }
    return totalActivations;
}

TEST(FailPoint, RandomActivationP0) {
    ASSERT_EQUALS(0, runParallelFailPointTest(FailPoint::random, 0, 1, 1000000));
}

TEST(FailPoint, RandomActivationP5) {
    ASSERT_APPROX_EQUAL(500000,
                        runParallelFailPointTest(
                            FailPoint::random, std::numeric_limits<int32_t>::max() / 2, 10, 100000),
                        1000);
}

TEST(FailPoint, RandomActivationP01) {
    ASSERT_APPROX_EQUAL(
        10000,
        runParallelFailPointTest(
            FailPoint::random, std::numeric_limits<int32_t>::max() / 100, 10, 100000),
        500);
}

TEST(FailPoint, RandomActivationP001) {
    ASSERT_APPROX_EQUAL(
        1000,
        runParallelFailPointTest(
            FailPoint::random, std::numeric_limits<int32_t>::max() / 1000, 10, 100000),
        500);
}

TEST(FailPoint, parseBSONEmptyFails) {
    auto swTuple = FailPoint::parseBSON(BSONObj());
    ASSERT_FALSE(swTuple.isOK());
}

TEST(FailPoint, parseBSONInvalidModeFails) {
    auto swTuple = FailPoint::parseBSON(BSON("missingModeField" << 1));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << 1));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << true));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << "notAMode"));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("invalidSubField" << 1)));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times" << "notAnInt")));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times" << -5)));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << "notADouble")));
    ASSERT_FALSE(swTuple.isOK());

    double greaterThan1 = 1.3;
    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << greaterThan1)));
    ASSERT_FALSE(swTuple.isOK());

    double lessThan1 = -0.3;
    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << lessThan1)));
    ASSERT_FALSE(swTuple.isOK());
}

TEST(FailPoint, parseBSONValidModeSucceeds) {
    auto swTuple = FailPoint::parseBSON(BSON("mode" << "off"));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << "alwaysOn"));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times" << 1)));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << 0.2)));
    ASSERT_TRUE(swTuple.isOK());
}

TEST(FailPoint, parseBSONInvalidDataFails) {
    auto swTuple = FailPoint::parseBSON(BSON("mode" << "alwaysOn"
                                                    << "data"
                                                    << "notABSON"));
    ASSERT_FALSE(swTuple.isOK());
}

TEST(FailPoint, parseBSONValidDataSucceeds) {
    auto swTuple = FailPoint::parseBSON(BSON("mode" << "alwaysOn"
                                                    << "data" << BSON("a" << 1)));
    ASSERT_TRUE(swTuple.isOK());
}

TEST(FailPoint, FailPointEnableBlockBasicTest) {
    auto failPoint = mongo::globalFailPointRegistry().find("dummy");

    ASSERT_FALSE(failPoint->shouldFail());

    {
        FailPointEnableBlock dummyFp("dummy");
        ASSERT_TRUE(failPoint->shouldFail());
    }

    ASSERT_FALSE(failPoint->shouldFail());
}

TEST(FailPoint, FailPointEnableBlockByPointer) {
    auto failPoint = mongo::globalFailPointRegistry().find("dummy");

    ASSERT_FALSE(failPoint->shouldFail());

    {
        FailPointEnableBlock dummyFp(failPoint);
        ASSERT_TRUE(failPoint->shouldFail());
    }

    ASSERT_FALSE(failPoint->shouldFail());
}

TEST(FailPoint, FailPointEnableBlockWithMode) {
    auto failPoint = mongo::globalFailPointRegistry().find("dummy");

    ASSERT_FALSE(failPoint->shouldFail());

    {
        FailPointEnableBlock dummyFp(
            "dummy", FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
        ASSERT_TRUE(failPoint->shouldFail());
        ASSERT_FALSE(failPoint->shouldFail());
    }

    ASSERT_FALSE(failPoint->shouldFail());
}

TEST(FailPoint, FailPointEnableBlockByPointerWithMode) {
    auto failPoint = mongo::globalFailPointRegistry().find("dummy");

    ASSERT_FALSE(failPoint->shouldFail());

    {
        FailPointEnableBlock dummyFp(
            failPoint, FailPoint::ModeOptions{.mode = FailPoint::Mode::nTimes, .val = 1});
        ASSERT_TRUE(failPoint->shouldFail());
        ASSERT_FALSE(failPoint->shouldFail());
    }

    ASSERT_FALSE(failPoint->shouldFail());
}

TEST(FailPoint, ExecuteIfBasicTest) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::nTimes, 1, BSON("skip" << true));
    {
        bool hit = false;
        failPoint.executeIf([](const BSONObj&) { ASSERT(!"shouldn't get here"); },
                            [&hit](const BSONObj& obj) {
                                hit = obj["skip"].trueValue();
                                return false;
                            });
        ASSERT(hit);
    }
    {
        bool hit = false;
        failPoint.executeIf(
            [&hit](const BSONObj& data) {
                hit = true;
                ASSERT(!data.isEmpty());
            },
            [](const BSONObj&) { return true; });
        ASSERT(hit);
    }
    failPoint.executeIf([](auto&&) { ASSERT(!"shouldn't get here"); }, [](auto&&) { return true; });
}
}  // namespace mongo_test

namespace mongo {

/**
 * Runs the given function with an operation context that has a deadline and asserts that
 * the function is interruptible.
 */
void assertFunctionInterruptible(std::function<void(Interruptible* interruptible)> f) {
    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    const auto service = ServiceContext::make(std::make_unique<SharedClockSourceAdapter>(mockClock),
                                              std::make_unique<SharedClockSourceAdapter>(mockClock),
                                              std::make_unique<TickSourceMock<>>());

    const auto client = service->getService()->makeClient("FailPointTest");
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Milliseconds{999}, ErrorCodes::ExceededTimeLimit);

    stdx::thread th([&] {
        ASSERT_THROWS_CODE(f(opCtx.get()), AssertionException, ErrorCodes::ExceededTimeLimit);
    });

    mockClock->advance(Milliseconds{1000});
    th.join();
}

TEST(FailPoint, PauseWhileSetInterruptibility) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);

    assertFunctionInterruptible(
        [&failPoint](Interruptible* interruptible) { failPoint.pauseWhileSet(interruptible); });

    failPoint.setMode(FailPoint::off);
}

TEST(FailPoint, PauseWhileSetCancelability) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);

    CancellationSource cs;
    CancellationToken ct = cs.token();
    cs.cancel();

    ASSERT_THROWS_CODE(failPoint.pauseWhileSetAndNotCanceled(Interruptible::notInterruptible(), ct),
                       DBException,
                       ErrorCodes::Interrupted);

    failPoint.setMode(FailPoint::off);
}

TEST(FailPoint, WaitForFailPointTimeout) {
    FailPoint failPoint("testFP");
    failPoint.setMode(FailPoint::alwaysOn);

    assertFunctionInterruptible([&failPoint](Interruptible* interruptible) {
        failPoint.waitForTimesEntered(interruptible, 1);
    });

    failPoint.setMode(FailPoint::off);
}

namespace {
// Turns an attempt to block into an exception, so tests can check wait thresholds without threads.
// Interrupt checks succeed; only the low-level wait returns an error for the wrapper to throw.
class ThrowOnBlock final : public Interruptible {
private:
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(stdx::condition_variable&,
                                                                         BasicLockableAdapter,
                                                                         Date_t) noexcept override {
        return Status(ErrorCodes::Interrupted, "Test wait would block");
    }

    Date_t getDeadline() const override {
        return Date_t::max();
    }

    Status checkForInterruptNoAssert() noexcept override {
        return Status::OK();
    }

    Status checkForDeadlineExpiredNoAssert(Date_t) noexcept override {
        return Status::OK();
    }

    DeadlineState pushArtificialDeadline(Date_t, ErrorCodes::Error) override {
        MONGO_UNREACHABLE;
    }

    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE;
    }

    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        return Date_t::now() + waitFor;
    }
};

void repeatedlyTestFailPoint(FailPoint& failPoint, int n) {
    failPoint.setMode(FailPoint::alwaysOn);
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(failPoint.shouldFail());
    }
    failPoint.setMode(FailPoint::off);
}
}  // namespace

// Test that we immediately return from 'waitForNNewEntries()' if the fail point has already been
// entered 'n' times.
TEST(FailPointBlock, WaitForNNewEntriesSuccessCase) {
    FailPoint failPoint("testFP");
    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(failPoint.shouldFail());
    }
    ASSERT_EQ(fpb.waitForNNewEntries(&interruptible, 3), 3);
}

// Test that we immediately return from 'waitForOneNewEntry()' if the fail point has already been
// entered once.
TEST(FailPointBlock, WaitForOneNewEntrySuccessCase) {
    FailPoint failPoint("testFP");
    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    ASSERT_TRUE(failPoint.shouldFail());
    ASSERT_EQ(fpb.waitForOneNewEntry(&interruptible), 1);
}

// Test that entries from before the block was created don't count towards the wait, so
// 'waitForOneNewEntry()' would block until a new entry occurs.
TEST(FailPointBlock, WaitForOneNewEntryIgnoresEntriesBeforeBlock) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);

    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    ASSERT_THROWS_CODE(
        fpb.waitForOneNewEntry(&interruptible), DBException, ErrorCodes::Interrupted);

    ASSERT_TRUE(failPoint.shouldFail());
    ASSERT_EQ(fpb.waitForOneNewEntry(&interruptible), 1);
}

// Test that 'waitForNNewEntries()' ignores entries from before the block was created and blocks
// until the fail point has been entered 'n' more times.
TEST(FailPointBlock, WaitForNNewEntriesIgnoresEntriesBeforeBlock) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);

    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    ASSERT_THROWS_CODE(
        fpb.waitForNNewEntries(&interruptible, 2), DBException, ErrorCodes::Interrupted);

    ASSERT_TRUE(failPoint.shouldFail());
    ASSERT_THROWS_CODE(
        fpb.waitForNNewEntries(&interruptible, 2), DBException, ErrorCodes::Interrupted);

    ASSERT_TRUE(failPoint.shouldFail());
    ASSERT_EQ(fpb.waitForNNewEntries(&interruptible, 2), 2);
}

TEST(FailPointBlock, WaitForOneNewEntryResumesAfterAnotherThreadEnters) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);
    FailPointEnableBlock fpb(&failPoint);

    const auto service = ServiceContext::make();
    const auto client = service->getService()->makeClient("FailPointWaiter");
    auto opCtx = client->makeOperationContext();
    // Bound the wait so a regression fails rather than leaving the test stuck in join().
    opCtx->setDeadlineAfterNowBy(Seconds{10}, ErrorCodes::ExceededTimeLimit);

    Atomic<bool> finished{false};
    FailPoint::EntryCountT entries = 0;
    std::exception_ptr waiterException;
    stdx::thread waiter([&] {
        try {
            entries = fpb.waitForOneNewEntry(opCtx.get());
        } catch (...) {
            waiterException = std::current_exception();
        }
        finished.store(true);
    });

    // Enter only after the waiter reaches an interruptible wait. Also stop polling if an
    // incorrect implementation returns early, so that failure doesn't cost the full deadline.
    const auto deadline = Date_t::now() + Seconds{10};
    while (!opCtx->isWaitingForConditionOrInterrupt() && !finished.load() &&
           Date_t::now() < deadline) {
        sleepmillis(1);
        LOGV2_DEBUG(13570302, 0, "Waiting for fail point to be entered");
    }
    const bool wasWaiting = opCtx->isWaitingForConditionOrInterrupt();
    const bool entered = failPoint.shouldFail();  // Unblocks the waiting thread.
    waiter.join();

    if (waiterException) {
        std::rethrow_exception(waiterException);
    }
    ASSERT_TRUE(wasWaiting);
    ASSERT_TRUE(entered);
    ASSERT_EQ(entries, 1);
}

// Test that entries made after the block was created but before the wait is called count towards
// the wait.
TEST(FailPointBlock, WaitCountsEntriesBeforeWaitIsCalled) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);

    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    ASSERT_TRUE(failPoint.shouldFail());
    ASSERT_EQ(fpb.waitForOneNewEntry(&interruptible), 1);
}

// Test that the waits return the number of entries since the block was created, not the absolute
// count, and not just the number that was waited for.
TEST(FailPointBlock, WaitReturnsEntriesSinceBlockCreated) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);

    FailPointEnableBlock fpb(&failPoint);
    ThrowOnBlock interruptible;
    ASSERT_EQ(fpb.waitForNNewEntries(&interruptible, 0), 0);
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(failPoint.shouldFail());
        ASSERT_EQ(fpb.waitForNNewEntries(&interruptible, 0), i);
    }
}

// Test that waiting for a negative number of entries trips a tassert rather than returning
// instantly.
TEST(FailPointBlock, WaitForNegativeEntriesTasserts) {
    FailPoint failPoint("testFP");
    FailPointEnableBlock fpb(&failPoint);
    ASSERT_TASSERT_CODE(fpb.waitForNNewEntries(-1), 13570300);
}

TEST(FailPointBlock, WaitForIntMaxEntriesTasserts) {
    FailPoint failPoint("testFP");
    repeatedlyTestFailPoint(failPoint, 3);
    FailPointEnableBlock fpb(&failPoint);
    ASSERT_TASSERT_CODE(fpb.waitForNNewEntries(std::numeric_limits<FailPoint::EntryCountT>::max()),
                        13570301);
    ASSERT_TASSERT_CODE(
        fpb.waitForNNewEntries(std::numeric_limits<FailPoint::EntryCountT>::max() - 1), 13570301);
    ASSERT_TASSERT_CODE(
        fpb.waitForNNewEntries(std::numeric_limits<FailPoint::EntryCountT>::max() - 2), 13570301);
}

// Test that the waits can be interrupted while the fail point has not yet been entered.
TEST(FailPointBlock, WaitForNewEntriesIsInterruptible) {
    FailPoint failPoint("testFP");
    FailPointEnableBlock fpb(&failPoint);

    assertFunctionInterruptible(
        [&fpb](Interruptible* interruptible) { fpb.waitForOneNewEntry(interruptible); });
    assertFunctionInterruptible(
        [&fpb](Interruptible* interruptible) { fpb.waitForNNewEntries(interruptible, 2); });
}

}  // namespace mongo
