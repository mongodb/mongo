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

TEST_F(MemoryUsageTrackerTest, RemainingMemoryUsageBytesReflectsAncestorLimit) {
    // Operation-wide tracker enforces a 100 byte cap; stage limit is much larger, so the ancestor
    // is the binding constraint and 'remaining' should reflect that, not the stage's own headroom.
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{100}};
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{10 * 1024}};

    stageTracker.add(50);
    // Stage has 10 * 1024 - 50 bytes of local headroom, but the op tracker only has 50 left.
    ASSERT_EQ(stageTracker.remainingMemoryUsageBytes(nullptr), 50);

    stageTracker.add(51);
    // Now over the op limit: remaining goes negative even though the stage's own limit isn't hit.
    ASSERT_EQ(stageTracker.remainingMemoryUsageBytes(nullptr), -1);
}

TEST_F(MemoryUsageTrackerTest, RemainingMemoryUsageBytesReflectsLocalLimitWhenTighter) {
    // Op limit is effectively unbounded, so the stage's own (tighter) limit is what binds.
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{std::numeric_limits<int64_t>::max()}};
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{100}};

    stageTracker.add(50);
    ASSERT_EQ(stageTracker.remainingMemoryUsageBytes(nullptr), 50);

    stageTracker.add(51);
    ASSERT_EQ(stageTracker.remainingMemoryUsageBytes(nullptr), -1);
}

TEST_F(MemoryUsageTrackerTest, RemainingMemoryUsageBytesOnStandaloneTrackerIsLocalOnly) {
    // No base: the chain walk collapses to the local check, same as withinMemoryLimit().
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(50);
    ASSERT_EQ(tracker.remainingMemoryUsageBytes(nullptr), 50);
    tracker.add(51);
    ASSERT_EQ(tracker.remainingMemoryUsageBytes(nullptr), -1);
}

TEST_F(MemoryUsageTrackerTest, RemainingMemoryUsageBytesTakesMinimumAcrossChain) {
    // A three-level chain where the middle tracker is the tightest: 'remaining' should reflect
    // the minimum across all levels, not just the immediate tracker or the root.
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{10 * 1024}};
    SimpleMemoryUsageTracker midTracker{&opTracker, MemoryUsageLimit{100}};
    SimpleMemoryUsageTracker leafTracker{&midTracker, MemoryUsageLimit{10 * 1024}};

    leafTracker.add(60);
    // op: 10*1024 - 60, mid: 100 - 60 = 40, leaf: 10*1024 - 60. The minimum (mid) is 40.
    ASSERT_EQ(leafTracker.remainingMemoryUsageBytes(nullptr), 40);
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
 * CurOp, and the root-only constructor so tests can latch a CurOp chunk size the way
 * OperationMemoryUsageTracker does.
 */
class TestableMemoryUsageTracker : public SimpleMemoryUsageTracker {
public:
    using SimpleMemoryUsageTracker::setIsOperationTracker;
    using SimpleMemoryUsageTracker::setWriteToCurOp;
    explicit TestableMemoryUsageTracker(MemoryUsageLimit limit) : SimpleMemoryUsageTracker(limit) {}
    TestableMemoryUsageTracker(MemoryUsageLimit limit, int64_t curOpWriteChunkSize)
        : SimpleMemoryUsageTracker(nullptr, limit, curOpWriteChunkSize) {}
};

TEST(SimpleMemoryUsageTrackerTest, NonRootTrackerUpdatesReportExactlyViaRoot) {
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;
    int64_t lastReportedInUse = -1;
    int64_t lastReportedPeak = -1;

    // The op-wide tracker is the root: no base, and no chunking of its own, so every change to it
    // reports to CurOp.
    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        ++curOpWriteCount;
        lastReportedInUse = inUse;
        lastReportedPeak = peak;
    });

    // A stage tracker has no '_writeToCurOp' of its own, so nothing gates on it; '_base' gets
    // every diff exactly, so the root reports on every one of this tracker's updates.
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{kBig}};

    stageTracker.add(50);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(opTracker.peakTrackedMemoryBytes(), 50);
    ASSERT_EQ(curOpWriteCount, 1);
    ASSERT_EQ(lastReportedInUse, 50);
    ASSERT_EQ(lastReportedPeak, 50);

    stageTracker.add(30);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 80);
    ASSERT_EQ(curOpWriteCount, 2);
    ASSERT_EQ(lastReportedInUse, 80);
}

TEST(SimpleMemoryUsageTrackerTest, MultiLevelChainPropagatesExactlyToRoot) {
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;
    int64_t lastReportedInUse = -1;

    // Chain: leaf -> mid -> root (reports to CurOp). Verifies that every diff propagates exactly
    // up the whole chain regardless of depth.
    TestableMemoryUsageTracker rootTracker{MemoryUsageLimit{kBig}};
    rootTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        ++curOpWriteCount;
        lastReportedInUse = inUse;
    });
    SimpleMemoryUsageTracker midTracker{&rootTracker, MemoryUsageLimit{kBig}};
    SimpleMemoryUsageTracker leafTracker{&midTracker, MemoryUsageLimit{kBig}};

    leafTracker.add(50);
    // Exact propagation all the way to the root, and the root -- being unchunked -- reports on
    // every update.
    ASSERT_EQ(rootTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(midTracker.inUseTrackedMemoryBytes(), 50);
    ASSERT_EQ(curOpWriteCount, 1);
    ASSERT_EQ(lastReportedInUse, 50);

    leafTracker.add(60);
    ASSERT_EQ(rootTracker.inUseTrackedMemoryBytes(), 110);
    ASSERT_EQ(curOpWriteCount, 2);
    ASSERT_EQ(lastReportedInUse, 110);
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsZeroWhenFullyReleased) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int64_t lastReportedInUse = -1;

    // Chunking is only meaningful on the tracker that owns '_writeToCurOp' -- here, the root
    // itself, which is also the one being updated directly (no separate stage tracker needed).
    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}, kChunkSize};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) { lastReportedInUse = inUse; });

    // Grow past a chunk boundary so CurOp holds a non-zero value.
    opTracker.add(150);
    ASSERT_EQ(lastReportedInUse, 150);

    // Release down into the first chunk: this crossing reports the exact value (30).
    opTracker.add(-120);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 30);
    ASSERT_EQ(lastReportedInUse, 30);

    // Fully release the remaining memory. This stays within the first chunk (no boundary crossing),
    // but CurOp must still be driven back to zero rather than left stale at 30, since a report has
    // already happened.
    opTracker.add(-30);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(lastReportedInUse, 0);
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsNothingBeforeFirstCrossing) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;

    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}, kChunkSize};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) { ++curOpWriteCount; });

    // Grow and fully release, never crossing the first chunk boundary. CurOp was never written to,
    // so there is nothing stale to correct -- unlike ChunkingReportsZeroWhenFullyReleased, the
    // return to zero here must not itself force a report.
    opTracker.add(50);
    opTracker.add(-50);
    ASSERT_EQ(opTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(curOpWriteCount, 0);
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingAfterReturnToZeroBehavesLikeNeverReported) {
    constexpr int64_t kChunkSize = 100;
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int curOpWriteCount = 0;
    int64_t lastReportedInUse = -1;

    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}, kChunkSize};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        ++curOpWriteCount;
        lastReportedInUse = inUse;
    });

    // Grow past the first boundary and release back to zero: one report for the crossing, one for
    // the return to zero, after which the tracker is back in the "never reported" state.
    opTracker.add(150);
    ASSERT_EQ(curOpWriteCount, 1);
    ASSERT_EQ(lastReportedInUse, 150);
    opTracker.add(-150);
    ASSERT_EQ(curOpWriteCount, 2);
    ASSERT_EQ(lastReportedInUse, 0);

    // From that state, sub-chunk growth must not report again -- the return to zero re-armed the
    // "never reported" state rather than leaving chunking behind.
    opTracker.add(50);
    ASSERT_EQ(curOpWriteCount, 2);

    // But crossing the first boundary reports again, with the exact total.
    opTracker.add(kChunkSize);
    ASSERT_EQ(curOpWriteCount, 3);
    ASSERT_EQ(lastReportedInUse, 150);

    // And the cycle repeats: releasing back to zero reports once more.
    opTracker.add(-150);
    ASSERT_EQ(curOpWriteCount, 4);
    ASSERT_EQ(lastReportedInUse, 0);
}


constexpr int64_t kChunkSizeForTest = 100;

/**
 * Walk usage across chunk boundaries in both directions, by one chunk and by several at once, and
 * assert that every crossing reports the exact total, while updates that stay within a chunk report
 * nothing.
 *
 * Chunks the root tracker directly, since chunking is only meaningful on the tracker that owns
 * '_writeToCurOp'.
 */
std::pair<int64_t, int64_t> getLastReportedValues(int64_t initialValue, int64_t nextValue) {
    constexpr int64_t kBig = 10 * 1024 * 1024;

    int64_t lastReportedInUse = -1;

    TestableMemoryUsageTracker opTracker{MemoryUsageLimit{kBig}, kChunkSizeForTest};
    opTracker.setWriteToCurOp([&](int64_t inUse, int64_t peak) { lastReportedInUse = inUse; });

    opTracker.add(initialValue);
    opTracker.add(nextValue);
    return {lastReportedInUse, opTracker.inUseTrackedMemoryBytes()};
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsInitialCrossing) {
    ASSERT_THAT(getLastReportedValues(0, kChunkSizeForTest + 20),
                std::pair(kChunkSizeForTest + 20, kChunkSizeForTest + 20))
        << "Single chunk up, from chunk [0, 100) to [100, 200).";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsUpwardCrossing) {
    ASSERT_THAT(getLastReportedValues(kChunkSizeForTest + 20, 8 * kChunkSizeForTest + 30),
                std::pair(9 * kChunkSizeForTest + 50, 9 * kChunkSizeForTest + 50))
        << "Several chunks up at once: [100, 200) to [900, 1000). Too far to step, so this "
           "divides.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingDoesNotReportNonCrossing) {
    ASSERT_THAT(getLastReportedValues(9 * kChunkSizeForTest + 50, 20),
                std::pair(9 * kChunkSizeForTest + 50, 9 * kChunkSizeForTest + 70))
        << "Within the new chunk: no crossing, no report.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsDownwardCrossing) {
    ASSERT_THAT(getLastReportedValues(9 * kChunkSizeForTest + 70, -kChunkSizeForTest),
                std::pair(8 * kChunkSizeForTest + 70, 8 * kChunkSizeForTest + 70))
        << "Single chunk down, to [800, 900).";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingDoesNotReportDownwardCrossingOnBoundary) {
    ASSERT_THAT(getLastReportedValues(8 * kChunkSizeForTest + 70, -70),
                std::pair(8 * kChunkSizeForTest + 70, 8 * kChunkSizeForTest))
        << "Exactly onto a boundary: 800 is the base of [800, 900), which is the chunk already "
           "reported, so this is not a crossing.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsDownwardCrossingFromBoundary) {
    ASSERT_THAT(getLastReportedValues(8 * kChunkSizeForTest, -1),
                std::pair(8 * kChunkSizeForTest - 1, 8 * kChunkSizeForTest - 1))
        << "One byte below that boundary crosses down into [700, 800).";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsDownwardCrossingAcrossMultipleChunks) {
    ASSERT_THAT(getLastReportedValues(8 * kChunkSizeForTest - 1, -7 * kChunkSizeForTest - 49),
                std::pair(50, 50))
        << "Several chunks down at once, back into the first chunk but not to zero.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingDoesNotReportGoingToZeroInSameChunk) {
    // Never crosses the first chunk boundary, so nothing is ever reported.
    ASSERT_THAT(getLastReportedValues(50, -50), std::pair(-1, 0))
        << "From within the first chunk down to zero.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsWhenCrossingMultipleBoundariesAtOnce) {
    ASSERT_THAT(getLastReportedValues(0, 50 * kChunkSizeForTest),
                std::pair(50 * kChunkSizeForTest, 50 * kChunkSizeForTest))
        << "Many chunk crossings up at once.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsWhenCrossingSingleBoundaryAtChunkSize) {
    ASSERT_THAT(getLastReportedValues(kChunkSizeForTest, kChunkSizeForTest),
                std::pair(2 * kChunkSizeForTest, 2 * kChunkSizeForTest))
        << "Single chunk crossing up, from boundary to next boundary.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingDoesNotReportWhenAddingOneBelowChunkSize) {
    ASSERT_THAT(getLastReportedValues(kChunkSizeForTest, kChunkSizeForTest - 1),
                std::pair(kChunkSizeForTest, 2 * kChunkSizeForTest - 1))
        << "Adding one less then chunkSize.";
}

TEST(SimpleMemoryUsageTrackerTest, ChunkingDoesNotReportWhenAddingZero) {
    ASSERT_THAT(getLastReportedValues(kChunkSizeForTest, 0),
                std::pair(kChunkSizeForTest, kChunkSizeForTest))
        << "Adding zero does not report.";
}

/**
 * A release from far above straight to zero crosses many chunks at once and lands exactly on the
 * boundary at zero. This is the one case where the divide-based bound and the return-to-zero rule
 * both apply, so it must still report exactly once.
 */
TEST(SimpleMemoryUsageTrackerTest, ChunkingReportsZeroWhenReleasedFromManyChunksAbove) {
    ASSERT_THAT(getLastReportedValues(5000, -5000), std::pair(0, 0))
        << "Many chunk crossings down at once, back to zero.";
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

/**
 * A class built on the token that changes its memory by a single delta, as a streams usage handle
 * does, must keep both inherited counters in step: the destructor releases both, so a counter that
 * is left at its constructor value leaves the tracker unbalanced. Instantiating the template over
 * the two-figure tracker also makes the compiler check the single-delta update against it.
 */
template <typename Tracker>
class SingleDeltaHandle : public MemoryUsageTokenImpl<Tracker> {
public:
    explicit SingleDeltaHandle(int64_t initial, Tracker* tracker)
        : MemoryUsageTokenImpl<Tracker>(initial, tracker, initial) {}

    void add(int64_t diff) {
        this->_curMemoryUsageBytes += diff;
        this->_curUniqueMemoryBytes += diff;
        this->_tracker->add(diff, diff);
    }
};

TEST_F(MemoryUsageTrackerTest, SingleDeltaHandleKeepsBothCountersInStepAndReleasesBoth) {
    {
        SingleDeltaHandle<SimpleMemoryUsageTracker> handle{10, &_funcTracker};
        handle.add(90);
        ASSERT_EQ(handle.getCurrentMemoryUsageBytes(), 100);
        ASSERT_EQ(handle.getCurrentUniqueMemoryBytes(), 100);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 100);

        handle.add(-60);
        ASSERT_EQ(handle.getCurrentUniqueMemoryBytes(), 40);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 40);
    }
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 0);
}

/**
 * A token holds two figures, everything it was charged and the part of it not already charged at
 * its allocation, and releases exactly those when it is destroyed.
 */
TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenHoldsAndReleasesBothFigures) {
    {
        MemoryUsageToken token{100LL, &_funcTracker, 40LL};
        ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 100LL);
        ASSERT_EQ(token.getCurrentUniqueMemoryBytes(), 40LL);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 40LL);

        token.add(30LL, 10LL);
        ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 130LL);
        ASSERT_EQ(token.getCurrentUniqueMemoryBytes(), 50LL);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 130LL);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 50LL);

        token.set(80LL, 20LL);
        ASSERT_EQ(token.getCurrentMemoryUsageBytes(), 80LL);
        ASSERT_EQ(token.getCurrentUniqueMemoryBytes(), 20LL);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 80LL);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 20LL);
    }
    // Destruction gives back both, not only what the constructor charged.
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 0);
}

/**
 * Moving a token carries both figures to the new owner, and a moved-from token releases nothing.
 */
TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenMoveCarriesBothFigures) {
    MemoryUsageToken token{100LL, &_funcTracker, 40LL};
    {
        MemoryUsageToken moved{std::move(token)};
        ASSERT_EQ(moved.getCurrentMemoryUsageBytes(), 100LL);
        ASSERT_EQ(moved.getCurrentUniqueMemoryBytes(), 40LL);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 40LL);

        MemoryUsageToken assigned;
        assigned = std::move(moved);
        ASSERT_EQ(assigned.getCurrentMemoryUsageBytes(), 100LL);
        ASSERT_EQ(assigned.getCurrentUniqueMemoryBytes(), 40LL);
        ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 100LL);
        ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 40LL);
    }
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 0);
}

/**
 * Move-assigning over a live token releases both of the figures the target held before it takes the
 * new ones.
 */
TEST_F(MemoryUsageTrackerTest, MemoryUsageTokenMoveAssignReleasesBothFiguresOfTarget) {
    MemoryUsageToken target{100LL, &_funcTracker, 40LL};
    MemoryUsageToken source{10LL, &_funcTracker, 3LL};
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 110LL);
    ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 43LL);

    target = std::move(source);
    ASSERT_EQ(target.getCurrentMemoryUsageBytes(), 10LL);
    ASSERT_EQ(target.getCurrentUniqueMemoryBytes(), 3LL);
    ASSERT_EQ(_funcTracker.inUseTrackedMemoryBytes(), 10LL);
    ASSERT_EQ(_funcTracker.inUseUniqueMemoryBytes(), 3LL);
}

/**
 * A token over a tracker with a separate operation-style root: the root sees the unique figure and
 * the stage the full one, and both come back on destruction.
 */
TEST(SimpleMemoryUsageTrackerTest, TokenFiguresReachOperationRootAsUniqueOnly) {
    constexpr int64_t kBig = 10 * 1024 * 1024;
    TestableMemoryUsageTracker root{MemoryUsageLimit{kBig}};
    root.setIsOperationTracker();
    SimpleMemoryUsageTracker stage{&root, MemoryUsageLimit{kBig}};
    {
        MemoryUsageToken token{100LL, &stage, 40LL};
        ASSERT_EQ(stage.inUseTrackedMemoryBytes(), 100);
        ASSERT_EQ(root.inUseTrackedMemoryBytes(), 40);
    }
    ASSERT_EQ(stage.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 0);
}

/**
 * A root that is an operation tracker reports and limit-checks the unique figure, while a root that
 * is not keeps the full one. Every tracker keeps both counters either way.
 */
TEST(SimpleMemoryUsageTrackerTest, OperationRootUsesUniqueFigureForLimitPeakAndReporting) {
    int64_t lastReportedInUse = -1;
    int64_t lastReportedPeak = -1;

    TestableMemoryUsageTracker root{MemoryUsageLimit{50}};
    root.setIsOperationTracker();
    root.setWriteToCurOp([&](int64_t inUse, int64_t peak) {
        lastReportedInUse = inUse;
        lastReportedPeak = peak;
    });
    SimpleMemoryUsageTracker stage{&root, MemoryUsageLimit{1000}};

    // 100 held, 40 not charged elsewhere: under the root's limit of 50 on the unique figure.
    stage.add(100, 40);
    ASSERT_TRUE(root.withinMemoryLimit(nullptr));
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 40);
    ASSERT_EQ(root.peakTrackedMemoryBytes(), 40);
    ASSERT_EQ(lastReportedInUse, 40);
    ASSERT_EQ(lastReportedPeak, 40);
    // Both counters are kept, whichever one the role reads.
    ASSERT_EQ(root.inUseUniqueMemoryBytes(), 40);

    // 20 more unique bytes take the operation over its limit of 50, though the stage's full figure
    // of 120 is far below the stage's own limit of 1000.
    stage.add(20, 20);
    ASSERT_FALSE(root.withinMemoryLimit(nullptr));
    ASSERT_FALSE(stage.withinMemoryLimit(nullptr));
    ASSERT_EQ(lastReportedInUse, 60);
    ASSERT_EQ(lastReportedPeak, 60);

    // Releasing it all drops the in-use figure and keeps the peak.
    stage.add(-120, -60);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(lastReportedInUse, 0);
    ASSERT_EQ(lastReportedPeak, 60);
}

TEST(SimpleMemoryUsageTrackerTest, NonOperationRootUsesFullFigure) {
    constexpr int64_t kBig = 10 * 1024 * 1024;
    TestableMemoryUsageTracker root{MemoryUsageLimit{kBig}};
    SimpleMemoryUsageTracker stage{&root, MemoryUsageLimit{kBig}};

    stage.add(100, 40);
    // Without the operation role, the root's own figure is the full one.
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 100);
    ASSERT_EQ(root.inUseUniqueMemoryBytes(), 40);
    stage.add(-100, -40);
}

/**
 * A multi-level chain gives each level its own full figure and the same unique figure.
 */
TEST(SimpleMemoryUsageTrackerTest, ChainKeepsFullPerLevelAndForwardsBothFigures) {
    constexpr int64_t kBig = 10 * 1024 * 1024;
    TestableMemoryUsageTracker root{MemoryUsageLimit{kBig}};
    root.setIsOperationTracker();
    SimpleMemoryUsageTracker middle{&root, MemoryUsageLimit{kBig}};
    SimpleMemoryUsageTracker leaf{&middle, MemoryUsageLimit{kBig}};

    leaf.add(100, 40);
    ASSERT_EQ(leaf.inUseTrackedMemoryBytes(), 100);
    ASSERT_EQ(middle.inUseTrackedMemoryBytes(), 100);
    ASSERT_EQ(middle.inUseUniqueMemoryBytes(), 40);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 40);

    leaf.add(-100, -40);
    ASSERT_EQ(middle.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 0);
}

/**
 * set() on a tracker with differing figures moves each by its own delta.
 */
TEST(SimpleMemoryUsageTrackerTest, SetMovesEachFigureByItsOwnDelta) {
    constexpr int64_t kBig = 10 * 1024 * 1024;
    TestableMemoryUsageTracker root{MemoryUsageLimit{kBig}};
    root.setIsOperationTracker();
    SimpleMemoryUsageTracker stage{&root, MemoryUsageLimit{kBig}};

    stage.set(100, 40);
    ASSERT_EQ(stage.inUseTrackedMemoryBytes(), 100);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 40);

    stage.set(70, 50);
    ASSERT_EQ(stage.inUseTrackedMemoryBytes(), 70);
    ASSERT_EQ(stage.inUseUniqueMemoryBytes(), 50);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 50);

    stage.set(0, 0);
    ASSERT_EQ(root.inUseTrackedMemoryBytes(), 0);
}

/**
 * A fresh tracker made from one that holds differing figures starts at zero in both.
 */
TEST(SimpleMemoryUsageTrackerTest, FreshTrackerStartsAtZeroInBothFigures) {
    constexpr int64_t kBig = 10 * 1024 * 1024;
    SimpleMemoryUsageTracker original{MemoryUsageLimit{kBig}};
    original.add(100, 40);

    auto fresh = original.makeFreshSimpleMemoryUsageTracker();
    ASSERT_EQ(fresh.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(fresh.inUseUniqueMemoryBytes(), 0);
    ASSERT_EQ(fresh.peakTrackedMemoryBytes(), 0);
    original.add(-100, -40);
}

}  // namespace
}  // namespace mongo
