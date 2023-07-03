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

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

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
            mongo::AtomicWord<bool> done{false};
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

    swTuple = FailPoint::parseBSON(BSON("mode"
                                        << "notAMode"));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("invalidSubField" << 1)));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times"
                                                       << "notAnInt")));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times" << -5)));
    ASSERT_FALSE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability"
                                                       << "notADouble")));
    ASSERT_FALSE(swTuple.isOK());

    double greaterThan1 = 1.3;
    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << greaterThan1)));
    ASSERT_FALSE(swTuple.isOK());

    double lessThan1 = -0.3;
    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << lessThan1)));
    ASSERT_FALSE(swTuple.isOK());
}

TEST(FailPoint, parseBSONValidModeSucceeds) {
    auto swTuple = FailPoint::parseBSON(BSON("mode"
                                             << "off"));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode"
                                        << "alwaysOn"));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("times" << 1)));
    ASSERT_TRUE(swTuple.isOK());

    swTuple = FailPoint::parseBSON(BSON("mode" << BSON("activationProbability" << 0.2)));
    ASSERT_TRUE(swTuple.isOK());
}

TEST(FailPoint, parseBSONInvalidDataFails) {
    auto swTuple = FailPoint::parseBSON(BSON("mode"
                                             << "alwaysOn"
                                             << "data"
                                             << "notABSON"));
    ASSERT_FALSE(swTuple.isOK());
}

TEST(FailPoint, parseBSONValidDataSucceeds) {
    auto swTuple = FailPoint::parseBSON(BSON("mode"
                                             << "alwaysOn"
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
    const auto service = ServiceContext::make();
    const std::shared_ptr<ClockSourceMock> mockClock = std::make_shared<ClockSourceMock>();
    service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));
    service->setPreciseClockSource(std::make_unique<SharedClockSourceAdapter>(mockClock));
    service->setTickSource(std::make_unique<TickSourceMock<>>());

    const auto client = service->makeClient("FailPointTest");
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

}  // namespace mongo
