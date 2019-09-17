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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

using mongo::BSONObj;
using mongo::FailPoint;
using mongo::FailPointEnableBlock;

namespace stdx = mongo::stdx;

namespace mongo_test {
TEST(FailPoint, InitialState) {
    FailPoint failPoint;
    ASSERT_FALSE(failPoint.shouldFail());
}

TEST(FailPoint, AlwaysOn) {
    FailPoint failPoint;
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
    FailPoint failPoint;
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
    FailPoint failPoint;
    bool called = false;
    failPoint.execute([&](const BSONObj&) { called = true; });
    ASSERT_FALSE(called);
}

TEST(FailPoint, BlockAlwaysOn) {
    FailPoint failPoint;
    failPoint.setMode(FailPoint::alwaysOn);
    bool called = false;

    failPoint.execute([&](const BSONObj&) { called = true; });

    ASSERT(called);
}

TEST(FailPoint, BlockNTimes) {
    FailPoint failPoint;
    failPoint.setMode(FailPoint::nTimes, 1);
    size_t counter = 0;

    for (size_t x = 0; x < 10; x++) {
        failPoint.execute([&](auto&&...) { counter++; });
    }

    ASSERT_EQUALS(1U, counter);
}

TEST(FailPoint, BlockWithException) {
    FailPoint failPoint;
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
    FailPoint failPoint;
    failPoint.setMode(FailPoint::alwaysOn, 0, BSON("x" << 20));

    failPoint.execute([&](const BSONObj& data) { ASSERT_EQUALS(20, data["x"].numberInt()); });
}

class FailPointStress : public mongo::unittest::Test {
public:
    void setUp() {
        _fp.setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
    }

    void tearDown() {
        // Note: This can loop indefinitely if reference counter was off
        _fp.setMode(FailPoint::off, 0, BSON("a" << 66));
    }

    void startTest() {
        ASSERT_EQUALS(0U, _tasks.size());

        _tasks.emplace_back(&FailPointStress::blockTask, this);
        _tasks.emplace_back(&FailPointStress::blockWithExceptionTask, this);
        _tasks.emplace_back(&FailPointStress::simpleTask, this);
        _tasks.emplace_back(&FailPointStress::flipTask, this);
    }

    void stopTest() {
        {
            stdx::lock_guard<mongo::Latch> lk(_mutex);
            _inShutdown = true;
        }
        for (auto& t : _tasks) {
            t.join();
        }
        _tasks.clear();
    }

private:
    void blockTask() {
        while (true) {
            _fp.execute([](const BSONObj& data) {
                // Expanded ASSERT_EQUALS since the error is not being
                // printed out properly
                if (data["a"].numberInt() != 44) {
                    mongo::error() << "blockTask thread detected anomaly"
                                   << " - data: " << data << std::endl;
                    ASSERT(false);
                }
            });

            stdx::lock_guard<mongo::Latch> lk(_mutex);
            if (_inShutdown)
                break;
        }
    }

    void blockWithExceptionTask() {
        while (true) {
            try {
                _fp.execute([](const BSONObj& data) {
                    if (data["a"].numberInt() != 44) {
                        mongo::error() << "blockWithExceptionTask thread detected anomaly"
                                       << " - data: " << data << std::endl;
                        ASSERT(false);
                    }

                    throw std::logic_error("blockWithExceptionTask threw");
                });
            } catch (const std::logic_error&) {
            }

            stdx::lock_guard<mongo::Latch> lk(_mutex);
            if (_inShutdown)
                break;
        }
    }

    void simpleTask() {
        while (true) {
            static_cast<void>(MONGO_unlikely(_fp.shouldFail()));
            stdx::lock_guard<mongo::Latch> lk(_mutex);
            if (_inShutdown)
                break;
        }
    }

    void flipTask() {
        while (true) {
            if (_fp.shouldFail()) {
                _fp.setMode(FailPoint::off, 0);
            } else {
                _fp.setMode(FailPoint::alwaysOn, 0, BSON("a" << 44));
            }

            stdx::lock_guard<mongo::Latch> lk(_mutex);
            if (_inShutdown)
                break;
        }
    }

    FailPoint _fp;
    std::vector<stdx::thread> _tasks;

    mongo::Mutex _mutex = MONGO_MAKE_LATCH();
    bool _inShutdown = false;
};

TEST_F(FailPointStress, Basic) {
    startTest();
    mongo::sleepsecs(30);
    stopTest();
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
    FailPoint failPoint;
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

TEST(FailPoint, FailPointBlockBasicTest) {
    auto failPoint = mongo::globalFailPointRegistry().find("dummy");

    ASSERT_FALSE(failPoint->shouldFail());

    {
        FailPointEnableBlock dummyFp("dummy");
        ASSERT_TRUE(failPoint->shouldFail());
    }

    ASSERT_FALSE(failPoint->shouldFail());
}

TEST(FailPoint, FailPointBlockIfBasicTest) {
    FailPoint failPoint;
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
