/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/memory_tracking/memory_usage_tracker.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MemoryUsageTrackerTest : public unittest::Test {
public:
    static constexpr auto kDefaultMax = 1 * 1024;  // 1KB max.
    MemoryUsageTrackerTest()
        : _tracker(false /** allowDiskUse */, kDefaultMax), _funcTracker(_tracker["funcTracker"]) {}


    MemoryUsageTracker _tracker;
    SimpleMemoryUsageTracker& _funcTracker;
};

TEST_F(MemoryUsageTrackerTest, FreshMemoryUsageTrackerInitializedCorrectly) {
    _tracker.add(50LL);

    MemoryUsageTracker freshMemoryUsageTracker = _tracker.makeFreshMemoryUsageTracker();

    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);

    ASSERT_EQ(freshMemoryUsageTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshMemoryUsageTracker.peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshMemoryUsageTracker.maxAllowedMemoryUsageBytes(),
              _tracker.maxAllowedMemoryUsageBytes());
}

TEST_F(MemoryUsageTrackerTest, FreshSimpleMemoryUsageTrackerInitializedCorrectly) {
    _funcTracker.add(50LL);

    SimpleMemoryUsageTracker freshSimpleMemoryUsageTracker =
        _funcTracker.makeFreshSimpleMemoryUsageTracker();

    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 50LL);

    ASSERT_EQ(freshSimpleMemoryUsageTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshSimpleMemoryUsageTracker.peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshSimpleMemoryUsageTracker.maxAllowedMemoryUsageBytes(),
              _funcTracker.maxAllowedMemoryUsageBytes());
}

TEST_F(MemoryUsageTrackerTest, FreshMemoryUsageTrackerCopiesBaseCorrectly) {
    SimpleMemoryUsageTracker memTrackerA =
        SimpleMemoryUsageTracker(nullptr, _tracker.maxAllowedMemoryUsageBytes());
    MemoryUsageTracker memTrackerB = MemoryUsageTracker(&memTrackerA, false, kDefaultMax);
    MemoryUsageTracker memTrackerC = memTrackerB.makeFreshMemoryUsageTracker();

    memTrackerB.add(50LL);
    memTrackerC.add(50LL);

    ASSERT_EQ(memTrackerA.inUseTrackedMemoryBytes(), 100LL);
    ASSERT_EQ(memTrackerA.peakTrackedMemoryBytes(), 100LL);

    ASSERT_EQ(memTrackerB.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(memTrackerB.peakTrackedMemoryBytes(), 50LL);

    ASSERT_EQ(memTrackerC.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(memTrackerC.peakTrackedMemoryBytes(), 50LL);
}

TEST_F(MemoryUsageTrackerTest, SetFunctionUsageUpdatesGlobal) {
    _tracker.add(50LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);

    // 50 global + 50 _funcTracker.
    _funcTracker.set(50);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 100LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 100LL);

    // New tracker adds another 50, 150 total.
    _tracker.set("newTracker", 50);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 150LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);

    // Lower usage of function tracker is reflected in global.
    _tracker.set("newTracker", 0);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 100LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
}

TEST_F(MemoryUsageTrackerTest, UpdateUsageUpdatesGlobal) {
    _tracker.add(50LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);

    // Add another 50 to the global, 100 total.
    _tracker.add(50);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 100LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 100LL);

    // Function tracker adds another 50, 150 total.
    _funcTracker.add(50);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 150LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);

    // Lower usage of function tracker is reflected in global.
    _funcTracker.add(-25);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 125LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
}

using MemoryUsageTrackerTestDeathTest = MemoryUsageTrackerTest;
DEATH_TEST_F(MemoryUsageTrackerTestDeathTest,
             UpdateFunctionUsageToNegativeIsDisallowed,
             "Underflow in memory tracking") {
    _funcTracker.set(50LL);
    _funcTracker.add(-100LL);
}

DEATH_TEST_F(MemoryUsageTrackerTestDeathTest,
             UpdateMemUsageToNegativeIsDisallowed,
             "Underflow in memory tracking") {
    _tracker.add(50LL);
    _tracker.add(-100LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenUpdatesCurrentAndMax) {
    {
        MemoryUsageToken token{50LL, &_tracker["subTracker"]};
        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);
        {
            MemoryUsageToken funcToken{100LL, &_funcTracker};
            ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
            ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

            ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 150LL);
            ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
        }
        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
    }
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenCanBeMoved) {
    {
        MemoryUsageToken token{50LL, &_tracker["subTracker"]};
        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);

        MemoryUsageToken token2(std::move(token));
        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);
    }
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenCanBeMoveAssigned) {
    {
        MemoryUsageToken token{50LL, &_tracker["subTracker"]};
        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 50LL);
        {
            MemoryUsageToken token2{100LL, &_funcTracker};
            ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
            ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

            ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 150LL);
            ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);

            token = std::move(token2);
            ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
            ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

            ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 100LL);
            ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
        }
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
        ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 100LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
    }
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenCanBeStoredInVector) {
    auto assertMemory = [this]() {
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
        ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 150LL);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
    };

    auto assertZeroMemory = [this]() {
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
        ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 100LL);

        ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 0);
        ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 150LL);
    };

    {
        std::vector<MemoryUsageToken> tokens;
        // Use default constructor
        tokens.resize(10);
        {
            std::vector<MemoryUsageToken> tokens2;
            tokens2.emplace_back(50LL, &_tracker["subTracker"]);
            tokens2.emplace_back(100LL, &_funcTracker);
            assertMemory();

            // Force reallocation
            tokens2.reserve(2 * tokens2.capacity());
            assertMemory();

            tokens = std::move(tokens2);
            assertMemory();
        }
        assertMemory();

        tokens.clear();
        assertZeroMemory();
    }
    assertZeroMemory();
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenAddUpdatesCurrentAndTracker) {
    MemoryUsageToken token{50LL, &_funcTracker};
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 50LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);

    token.add(30LL);
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 80LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 80LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 80LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 80LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenAddNegativeDecreasesCurrentAndTracker) {
    MemoryUsageToken token{80LL, &_funcTracker};
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 80LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 80LL);

    token.add(-30LL);
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 50LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 80LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenAddWithNullTrackerIsNoOp) {
    MemoryUsageToken token;
    token.add(100LL);
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 0LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenSetIncreasesCurrentAndTracker) {
    MemoryUsageToken token{50LL, &_funcTracker};
    token.set(80LL);
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 80LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 80LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 80LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 80LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenSetDecreasesCurrentAndTracker) {
    MemoryUsageToken token{80LL, &_funcTracker};
    token.set(30LL);
    ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 30LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 30LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 30LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 80LL);
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenSetReleasesCorrectAmountOnDestruction) {
    {
        MemoryUsageToken token{50LL, &_funcTracker};
        token.set(80LL);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 80LL);
    }
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0LL);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 80LL);
    ASSERT_EQ(_tracker.inUseTrackedMemoryBytes(), 0LL);
    ASSERT_EQ(_tracker.peakTrackedMemoryBytes(), 80LL);
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitChecksAncestorLimit) {
    // Operation-wide tracker enforces a 100 byte cap; stage limit is much larger.
    SimpleMemoryUsageTracker opTracker{100 /* maxAllowedMemoryUsageBytes */};
    SimpleMemoryUsageTracker stageTracker{&opTracker, 10 * 1024 /* maxAllowedMemoryUsageBytes */};

    stageTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit());

    // Stage usage exceeds the op limit but not its own stage limit. The chain check still
    // catches the breach at the ancestor level.
    stageTracker.add(51);
    ASSERT_FALSE(stageTracker.withinMemoryLimit());

    stageTracker.add(-51);
    ASSERT_TRUE(stageTracker.withinMemoryLimit());
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitAlsoChecksLocalLimit) {
    // Op limit is effectively unbounded, but the stage limit is tight. A local breach must
    // still fail withinMemoryLimit().
    SimpleMemoryUsageTracker opTracker{std::numeric_limits<int64_t>::max()};
    SimpleMemoryUsageTracker stageTracker{&opTracker, 100 /* maxAllowedMemoryUsageBytes */};

    stageTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit());

    stageTracker.add(51);
    ASSERT_FALSE(stageTracker.withinMemoryLimit());
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitOnStandaloneTrackerIsLocalOnly) {
    // No base: chain check collapses to the local check.
    SimpleMemoryUsageTracker tracker{100 /* maxAllowedMemoryUsageBytes */};
    tracker.add(50);
    ASSERT_TRUE(tracker.withinMemoryLimit());
    tracker.add(51);
    ASSERT_FALSE(tracker.withinMemoryLimit());
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitOnMemoryUsageTracker) {
    // MemoryUsageTracker is the per-function variant whose internal _baseTracker is linked to
    // the op-wide tracker. The forwarder should pick up the op-wide breach too.
    SimpleMemoryUsageTracker opTracker{100 /* maxAllowedMemoryUsageBytes */};
    MemoryUsageTracker stageTracker{
        &opTracker, false /* allowDiskUse */, 10 * 1024 /* maxMemoryUsageBytes */};
    SimpleMemoryUsageTracker& funcTracker = stageTracker["fn"];

    funcTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit());

    funcTracker.add(51);
    // Stage's own roll-up is still within the stage limit; op tally exceeds the op limit.
    ASSERT_FALSE(stageTracker.withinMemoryLimit());
}

TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenWith) {
    static const std::vector<std::string> kLines = {"a", "bb", "ccc", "dddd"};

    int64_t total_size = 0;
    std::vector<MemoryUsageTokenWith<std::string>> memory_tracked_vector;
    for (const auto& line : kLines) {
        memory_tracked_vector.emplace_back(MemoryUsageToken{line.size(), &_tracker["subTracker"]},
                                           line);
        total_size += line.size();
        ASSERT_EQ(total_size, _tracker.inUseTrackedMemoryBytes());
        ASSERT_EQ(total_size, _tracker.peakTrackedMemoryBytes());
    }

    int64_t max_size = total_size;
    while (!memory_tracked_vector.empty()) {
        total_size -= memory_tracked_vector.back().value().size();
        memory_tracked_vector.pop_back();
        ASSERT_EQ(total_size, _tracker.inUseTrackedMemoryBytes());
        ASSERT_EQ(max_size, _tracker.peakTrackedMemoryBytes());
    }
}

class DeduplicatorReporterTest : public unittest::Test {
public:
    DeduplicatorReporterTest()
        : _reporter(
              [this](int64_t bytesDelta, int64_t recordsDelta) {
                  _lastBytesDelta = bytesDelta;
                  _lastRecordsDelta = recordsDelta;
                  ++_callbackCount;
              },
              /* chunkSize = */ 1) {}

protected:
    int64_t _lastBytesDelta = 0;
    int64_t _lastRecordsDelta = 0;
    int _callbackCount = 0;
    DeduplicatorReporter _reporter;
};

TEST_F(DeduplicatorReporterTest, DefaultRecordsDiffIsOne) {
    _reporter.add(100);
    ASSERT_EQ(_callbackCount, 1);
    ASSERT_EQ(_lastBytesDelta, 100);
    ASSERT_EQ(_lastRecordsDelta, 1);
}

TEST_F(DeduplicatorReporterTest, ExplicitRecordsDiffIsReported) {
    _reporter.add(100, 5);
    ASSERT_EQ(_callbackCount, 1);
    ASSERT_EQ(_lastBytesDelta, 100);
    ASSERT_EQ(_lastRecordsDelta, 5);
}

TEST_F(DeduplicatorReporterTest, NegativeRecordsDiffDecrementsCount) {
    _reporter.add(100);
    _reporter.add(50, -1);
    ASSERT_EQ(_callbackCount, 2);
    ASSERT_EQ(_lastBytesDelta, 50);
    ASSERT_EQ(_lastRecordsDelta, -1);
}

TEST_F(DeduplicatorReporterTest, NoCallbackWhenBytesUnchanged) {
    _reporter.add(0, 5);
    ASSERT_EQ(_callbackCount, 0);
}

TEST_F(DeduplicatorReporterTest, RecordsDeltaAccumulatesUntilChunkCrossing) {
    DeduplicatorReporter reporter(
        [this](int64_t bytesDelta, int64_t recordsDelta) {
            _lastBytesDelta = bytesDelta;
            _lastRecordsDelta = recordsDelta;
            ++_callbackCount;
        },
        100);

    reporter.add(50, 3);  // bytes=50, within chunk [0,100), no callback
    ASSERT_EQ(_callbackCount, 0);

    reporter.add(100, 2);  // bytes=150, crosses into chunk [100,200), reports accumulated delta
    ASSERT_EQ(_callbackCount, 1);
    ASSERT_EQ(_lastBytesDelta, 100);
    ASSERT_EQ(_lastRecordsDelta, 5);
}

using DeduplicatorReporterTestDeathTest = DeduplicatorReporterTest;
DEATH_TEST_F(DeduplicatorReporterTestDeathTest,
             RecordCountUnderflowIsDisallowed,
             "Underflow in record count tracking") {
    _reporter.add(100);    // count = 1
    _reporter.add(0, -2);  // would bring count to -1
}

DEATH_TEST_F(DeduplicatorReporterTestDeathTest,
             MemoryUnderflowIsDisallowed,
             "Underflow in memory tracking") {
    _reporter.add(100);   // bytes = 100
    _reporter.add(-200);  // would bring bytes to -100
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitDoesNotThrowWhenUnderLimit) {
    SimpleMemoryUsageTracker tracker{1000};
    tracker.add(500);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit("$testExpr"));
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitDoesNotThrowWhenAtLimit) {
    SimpleMemoryUsageTracker tracker{1000};
    tracker.add(1000);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit("$testExpr"));
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitThrowsWhenOverLimit) {
    SimpleMemoryUsageTracker tracker{100};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$testExpr");
        ASSERT_STRING_CONTAINS(ex.reason(), "200");  // current usage
        ASSERT_STRING_CONTAINS(ex.reason(), "100");  // local limit
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitIncludesGlobalLimitWhenBaseIsSet) {
    SimpleMemoryUsageTracker base{5000};
    SimpleMemoryUsageTracker tracker{&base, 100};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "100");   // local limit
        ASSERT_STRING_CONTAINS(ex.reason(), "5000");  // global limit from base
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitThrowsWhenOnlyGlobalLimitExceeded) {
    SimpleMemoryUsageTracker base{100};
    SimpleMemoryUsageTracker tracker{&base, 10 * 1024};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "Global memory limit: 100");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitOmitsGlobalLimitWhenNoBase) {
    SimpleMemoryUsageTracker tracker{100};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "100");
        ASSERT_STRING_OMITS(ex.reason(), "Global memory limit");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitIncludesStageNameWhenProvided) {
    SimpleMemoryUsageTracker tracker{100};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr", "$group");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$testExpr");
        ASSERT_STRING_CONTAINS(ex.reason(), "Stage: $group");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitOmitsStageNameWhenNotProvided) {
    SimpleMemoryUsageTracker tracker{100};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit("$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_OMITS(ex.reason(), "Stage:");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitLogsErrorWhenOverLimit) {
    unittest::LogCaptureGuard logs{};
    SimpleMemoryUsageTracker tracker{100};
    tracker.add(200);
    ASSERT_THROWS_CODE(tracker.assertWithinMemoryLimit("$testExpr", "$group"),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
    logs.stop();
    bool foundLog = false;
    for (const auto& line : logs.getBSON()) {
        if (line["id"].numberLong() == 12932700) {
            foundLog = true;
            ASSERT_STRING_CONTAINS(line["attr"].Obj()["error"].String(), "Stage: $group");
        }
    }
    ASSERT(foundLog);
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitDoesNotLogWhenUnderLimit) {
    unittest::LogCaptureGuard logs{};
    SimpleMemoryUsageTracker tracker{1000};
    tracker.add(500);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit("$testExpr", "$group"));
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 12932700)), 0);
}

}  // namespace
}  // namespace mongo
