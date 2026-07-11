// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/memory_usage_tracker.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

// Reads the process-wide metrics.query.operationsFailedDueToMemoryLimit counter out of the
// serverStatus metric tree. We navigate to and serialize only this one metric (rather than
// appending the entire tree) since other registered metrics may require a running global
// ServiceContext. Returns none when the metric is absent, e.g. on build variants without
// OpenTelemetry.
boost::optional<long long> readOperationsFailedDueToMemoryLimit() {
    const MetricTree::ChildMap* children = &globalMetricTreeSet()[ClusterRole::None].children();
    const MetricTree::TreeNode* leaf = nullptr;
    for (std::string_view component :
         {"metrics"sv, "query"sv, "operationsFailedDueToMemoryLimit"sv}) {
        auto it = children->find(component);
        if (it == children->end()) {
            return boost::none;
        }
        if (component == "operationsFailedDueToMemoryLimit"sv) {
            leaf = &it->second;
            break;
        }
        if (!it->second.isSubtree()) {
            return boost::none;
        }
        children = &it->second.getSubtree()->children();
    }
    if (!leaf || leaf->isSubtree()) {
        return boost::none;
    }

    BSONObjBuilder bob;
    leaf->getMetric()->appendTo(bob, "operationsFailedDueToMemoryLimit");
    BSONObj obj = bob.obj();
    BSONElement el = obj.getField("operationsFailedDueToMemoryLimit");
    if (el.eoo()) {
        return boost::none;
    }
    return el.Long();
}


class MemoryUsageTrackerTest : public unittest::Test {
public:
    static constexpr auto kDefaultMax = 1 * 1024;  // 1KB max.
    MemoryUsageTrackerTest()
        : _tracker(false /** allowDiskUse */, MemoryUsageLimit{kDefaultMax}),
          _funcTracker(_tracker["funcTracker"]) {}


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
    ASSERT_EQ(freshMemoryUsageTracker.maxAllowedMemoryUsageBytes(nullptr),
              _tracker.maxAllowedMemoryUsageBytes(nullptr));
}

TEST_F(MemoryUsageTrackerTest, FreshSimpleMemoryUsageTrackerInitializedCorrectly) {
    _funcTracker.add(50LL);

    SimpleMemoryUsageTracker freshSimpleMemoryUsageTracker =
        _funcTracker.makeFreshSimpleMemoryUsageTracker();

    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 50LL);
    ASSERT_EQ(_funcTracker.peakTrackedMemoryBytes(), 50LL);

    ASSERT_EQ(freshSimpleMemoryUsageTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshSimpleMemoryUsageTracker.peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(freshSimpleMemoryUsageTracker.maxAllowedMemoryUsageBytes(nullptr),
              _funcTracker.maxAllowedMemoryUsageBytes(nullptr));
}

TEST_F(MemoryUsageTrackerTest, FreshMemoryUsageTrackerCopiesBaseCorrectly) {
    SimpleMemoryUsageTracker memTrackerA = SimpleMemoryUsageTracker(
        nullptr, MemoryUsageLimit{_tracker.maxAllowedMemoryUsageBytes(nullptr)});
    MemoryUsageTracker memTrackerB =
        MemoryUsageTracker(&memTrackerA, false, MemoryUsageLimit{kDefaultMax});
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
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{100}};
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{10 * 1024}};

    stageTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit(nullptr));

    // Stage usage exceeds the op limit but not its own stage limit. The chain check still
    // catches the breach at the ancestor level.
    stageTracker.add(51);
    ASSERT_FALSE(stageTracker.withinMemoryLimit(nullptr));

    stageTracker.add(-51);
    ASSERT_TRUE(stageTracker.withinMemoryLimit(nullptr));
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitAlsoChecksLocalLimit) {
    // Op limit is effectively unbounded, but the stage limit is tight. A local breach must
    // still fail withinMemoryLimit().
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{std::numeric_limits<int64_t>::max()}};
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{100}};

    stageTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit(nullptr));

    stageTracker.add(51);
    ASSERT_FALSE(stageTracker.withinMemoryLimit(nullptr));
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitOnStandaloneTrackerIsLocalOnly) {
    // No base: chain check collapses to the local check.
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(50);
    ASSERT_TRUE(tracker.withinMemoryLimit(nullptr));
    tracker.add(51);
    ASSERT_FALSE(tracker.withinMemoryLimit(nullptr));
}

TEST_F(MemoryUsageTrackerTest, WithinMemoryLimitOnMemoryUsageTracker) {
    // MemoryUsageTracker is the per-function variant whose internal _baseTracker is linked to
    // the op-wide tracker. The forwarder should pick up the op-wide breach too.
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{100}};
    MemoryUsageTracker stageTracker{&opTracker,
                                    false /* allowDiskUse */,
                                    MemoryUsageLimit{10 * 1024} /* maxMemoryUsageBytes */};
    SimpleMemoryUsageTracker& funcTracker = stageTracker["fn"];

    funcTracker.add(50);
    ASSERT_TRUE(stageTracker.withinMemoryLimit(nullptr));

    funcTracker.add(51);
    // Stage's own roll-up is still within the stage limit; op tally exceeds the op limit.
    ASSERT_FALSE(stageTracker.withinMemoryLimit(nullptr));
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

/**
 * Test-only subclass that exposes setWriteToCurOp() so tests can observe what gets reported to
 * CurOp.
 */
class TestableMemoryUsageTracker : public SimpleMemoryUsageTracker {
public:
    using SimpleMemoryUsageTracker::setWriteToCurOp;
    using SimpleMemoryUsageTracker::SimpleMemoryUsageTracker;
};

TEST(SimpleMemoryUsageTrackerTest, ChunkingDecouplesBasePropagationFromCurOpReporting) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;
    int64_t lastReportedInUse = -1;
    int64_t lastReportedPeak = -1;

    // The op-wide tracker is the root: no base, but it reports to CurOp.
    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        ++curOpWriteCount;
        lastReportedInUse = inUse;
        lastReportedPeak = peak;
    });

    // The stage tracker chunks its reporting to CurOp.
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{kBig}, kChunkSize};

    // Within the first chunk [0, 100): the op-wide tracker must see the exact total, but nothing
    // is reported to CurOp yet.
    stageTracker.add(50);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(opTracker.peakTrackedMemoryBytes(), 50);
    ASSERT_EQ(curOpWriteCount, 0);

    // Crossing into chunk [100, 200) triggers a single CurOp write. Both the base total and the
    // value reported to CurOp are exact (110)
    stageTracker.add(60);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 110);
    ASSERT_EQ(opTracker.peakTrackedMemoryBytes(), 110);
    ASSERT_EQ(curOpWriteCount, 1);
    ASSERT_EQ(lastReportedInUse, 110);
    ASSERT_EQ(lastReportedPeak, 110);

    // Another update that stays within chunk [100, 200) propagates exactly to the base but does
    // not write to CurOp.
    stageTracker.add(30);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 140);
    ASSERT_EQ(curOpWriteCount, 1);

    // Releasing memory back across a chunk boundary reports again with the exact in-use total (50).
    stageTracker.add(-90);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(curOpWriteCount, 2);
    ASSERT_EQ(lastReportedInUse, 50);
    // Peak is exact and monotonically non-decreasing.
    ASSERT_EQ(lastReportedPeak, 140);
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingOnIntermediateTrackerStillPropagatesExactlyToRoot) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;
    int64_t lastReportedInUse = -1;

    // Chain: leaf (no chunking) -> mid (chunking) -> root (reports to CurOp).
    TestableMemoryUsageTracker rootTracker{MemoryUsageLimit{kBig}};
    rootTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        ++curOpWriteCount;
        lastReportedInUse = inUse;
    });
    SimpleMemoryUsageTracker midTracker{&rootTracker, MemoryUsageLimit{kBig}, kChunkSize};
    SimpleMemoryUsageTracker leafTracker{&midTracker, MemoryUsageLimit{kBig}};

    leafTracker.add(50);
    // Exact propagation all the way to the root, but no CurOp write within the first chunk.
    ASSERT_EQ(rootTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(midTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(curOpWriteCount, 0);

    leafTracker.add(60);  // mid crosses into [100, 200)
    ASSERT_EQ(rootTracker.inUseTrackedMemoryBytes(), 110);
    ASSERT_EQ(curOpWriteCount, 1);
    // CurOp reports the exact in-use total (110), gated by the mid tracker's chunk crossing.
    ASSERT_EQ(lastReportedInUse, 110);
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsZeroWhenFullyReleased) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int64_t lastReportedInUse = -1;

    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) { lastReportedInUse = inUse; });
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{kBig}, kChunkSize};

    // Grow past a chunk boundary so CurOp holds a non-zero value.
    stageTracker.add(150);
    ASSERT_EQ(lastReportedInUse, 150);

    // Release down into the first chunk: this crossing reports the exact value (30).
    stageTracker.add(-120);
    ASSERT_EQ(stageTracker.inUseTrackedMemoryBytes(), 30);
    ASSERT_EQ(lastReportedInUse, 30);

    // Fully release the remaining memory. This stays within the first chunk (no boundary crossing),
    // but CurOp must still be driven back to zero rather than left stale at 30.
    stageTracker.add(-30);
    ASSERT_EQ(stageTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(lastReportedInUse, 0);
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitDoesNotThrowWhenUnderLimit) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1000}};
    tracker.add(500);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit(nullptr, "$testExpr"));
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitDoesNotThrowWhenAtLimit) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1000}};
    tracker.add(1000);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit(nullptr, "$testExpr"));
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitThrowsWhenOverLimit) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$testExpr");
        ASSERT_STRING_CONTAINS(ex.reason(), "200");  // current usage
        ASSERT_STRING_CONTAINS(ex.reason(), "100");  // local limit
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitIncludesGlobalLimitWhenBaseIsSet) {
    SimpleMemoryUsageTracker base{MemoryUsageLimit{5000}};
    SimpleMemoryUsageTracker tracker{&base, MemoryUsageLimit{100}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "100");   // local limit
        ASSERT_STRING_CONTAINS(ex.reason(), "5000");  // global limit from base
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitThrowsWhenOnlyGlobalLimitExceeded) {
    SimpleMemoryUsageTracker base{MemoryUsageLimit{100}};
    SimpleMemoryUsageTracker tracker{&base, MemoryUsageLimit{10 * 1024}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "Global memory limit: 100");
        // The overflowing level is the base, so surface how much memory the base is actually using
        // (200).
        ASSERT_STRING_CONTAINS(ex.reason(), "Global memory used: 200");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitReportsUsageForIntermediateLevel) {
    // Three-level chain (leaf -> intermediate -> global root) where the intermediate tracker is the
    // one that overflows: the leaf and root limits are generous, the intermediate limit is small.
    SimpleMemoryUsageTracker root{MemoryUsageLimit{100000}};
    SimpleMemoryUsageTracker intermediate{&root, MemoryUsageLimit{100}};
    SimpleMemoryUsageTracker tracker{&intermediate, MemoryUsageLimit{100000}};
    // add() propagates up the chain, so every tracker ends up holding 200 bytes.
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        // The overflowing tracker is the intermediate base, so its in-use memory (200) and limit
        // (100) must be surfaced under a "Level N" entry rather than only the leaf's usage.
        ASSERT_STRING_CONTAINS(ex.reason(),
                               "Level 1 memory used: 200 bytes. Level 1 memory limit: 100 bytes");
        // The root beyond the intermediate is still reported as the global level.
        ASSERT_STRING_CONTAINS(ex.reason(), "Global memory used: 200");
        ASSERT_STRING_CONTAINS(ex.reason(), "Global memory limit: 100000");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitOmitsGlobalLimitWhenNoBase) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "100");
        ASSERT_STRING_OMITS(ex.reason(), "Global memory limit");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitIncludesStageNameWhenProvided) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr", "$group");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$testExpr");
        ASSERT_STRING_CONTAINS(ex.reason(), "Stage: $group");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitOmitsStageNameWhenNotProvided) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    try {
        tracker.assertWithinMemoryLimit(nullptr, "$testExpr");
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_OMITS(ex.reason(), "Stage:");
    }
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitLogsErrorWhenOverLimit) {
    unittest::LogCaptureGuard logs{};
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    ASSERT_THROWS_CODE(tracker.assertWithinMemoryLimit(nullptr, "$testExpr", "$group"),
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
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1000}};
    tracker.add(500);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit(nullptr, "$testExpr", "$group"));
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 12932700)), 0);
}

TEST(SimpleMemoryUsageTrackerTest, AssertWithinMemoryLimitIncrementsFailureMetricWhenOverLimit) {
    auto before = readOperationsFailedDueToMemoryLimit();
    if (!before) {
        // serverStatus surfacing of OpenTelemetry metrics is unavailable on this build variant.
        return;
    }

    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(200);
    ASSERT_THROWS_CODE(tracker.assertWithinMemoryLimit(nullptr, "$testExpr"),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);

    // The counter is process-wide and monotonically non-decreasing, so assert it advanced by
    // exactly one relative to the value observed before the failure.
    ASSERT_EQ(*readOperationsFailedDueToMemoryLimit(), *before + 1);
}

TEST(SimpleMemoryUsageTrackerTest,
     AssertWithinMemoryLimitDoesNotIncrementFailureMetricWhenUnderLimit) {
    auto before = readOperationsFailedDueToMemoryLimit();
    if (!before) {
        // serverStatus surfacing of OpenTelemetry metrics is unavailable on this build variant.
        return;
    }

    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1000}};
    tracker.add(500);
    ASSERT_DOES_NOT_THROW(tracker.assertWithinMemoryLimit(nullptr, "$testExpr"));

    ASSERT_EQ(*readOperationsFailedDueToMemoryLimit(), *before);
}

}  // namespace
}  // namespace mongo
