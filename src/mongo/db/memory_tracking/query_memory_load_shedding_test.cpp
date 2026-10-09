// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/query_memory_load_shedding.h"

#include "mongo/db/admission/execution_control/execution_control_parameters_gen.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using query_memory_load_shedding_detail::shedProbability;

// Common inputs: 1 GiB limit, low mark 80%, high mark 100%, 32 MiB size reference. With these marks
// RSS at 90% -> pressure 0.5 (odds 1) and RSS at 95% -> pressure 0.75 (odds 3).
constexpr int64_t kMemLimit = 1LL << 30;
constexpr int64_t kSizeRef = 32LL << 20;
constexpr int32_t kLow = 80;
constexpr int32_t kHigh = 100;

int64_t rssAtPercent(int pct) {
    return kMemLimit * pct / 100;
}

TEST(QueryMemoryLoadSheddingProbability, DisabledReturnsZero) {
    ASSERT_EQ(0.0,
              shedProbability(rssAtPercent(90),
                              kMemLimit,
                              kSizeRef,
                              /*lowMarkPercent=*/-1,
                              kHigh,
                              kSizeRef,
                              Seconds(1)));
}

TEST(QueryMemoryLoadSheddingProbability, AtOrBelowLowMarkReturnsZero) {
    ASSERT_EQ(
        0.0,
        shedProbability(rssAtPercent(80), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1)));
    ASSERT_EQ(
        0.0,
        shedProbability(rssAtPercent(50), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1)));
}

TEST(QueryMemoryLoadSheddingProbability, AtOrAboveHighMarkReturnsOne) {
    // 100% shedding at/above the high mark, regardless of size or dt -- even a tiny op.
    ASSERT_EQ(
        1.0,
        shedProbability(rssAtPercent(100), kMemLimit, 1024, kLow, kHigh, kSizeRef, Seconds(1)));
    ASSERT_EQ(1.0,
              shedProbability(
                  rssAtPercent(100), kMemLimit, 1024, kLow, kHigh, kSizeRef, Milliseconds(1)));
}

TEST(QueryMemoryLoadSheddingProbability, NoTrackedMemoryReturnsZero) {
    ASSERT_EQ(0.0,
              shedProbability(rssAtPercent(90),
                              kMemLimit,
                              /*opTrackedBytes=*/0,
                              kLow,
                              kHigh,
                              kSizeRef,
                              Seconds(1)));
}

TEST(QueryMemoryLoadSheddingProbability, ReferenceSizeAtHalfPressureIs50PercentPerSecond) {
    // A reference-size op at mid-pressure has a 50% shed chance per second (one-second half-life).
    const double p =
        shedProbability(rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1));
    ASSERT_APPROX_EQUAL(p, 0.5, 1e-6);
}

TEST(QueryMemoryLoadSheddingProbability, SizeScalesHazardAndIsUnbounded) {
    // At pressure 0.5, hazard = ln(2) * (opBytes / sizeRef), unbounded, so p = 1 -
    // 2^-(opBytes/ref).
    const double half = shedProbability(
        rssAtPercent(90), kMemLimit, kSizeRef / 2, kLow, kHigh, kSizeRef, Seconds(1));
    const double ref =
        shedProbability(rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1));
    const double huge = shedProbability(
        rssAtPercent(90), kMemLimit, kSizeRef * 8, kLow, kHigh, kSizeRef, Seconds(1));
    ASSERT_APPROX_EQUAL(half, 1.0 - std::pow(2.0, -0.5), 1e-6);
    ASSERT_APPROX_EQUAL(huge, 1.0 - std::pow(2.0, -8.0), 1e-6);
    // Larger operations are strictly more likely to be shed (no clamp at the reference size).
    ASSERT_LT(half, ref);
    ASSERT_LT(ref, huge);
}

TEST(QueryMemoryLoadSheddingProbability, HigherPressureRaisesProbability) {
    // pressure 0.75 -> odds 3, vs pressure 0.5 -> odds 1, at the reference size: p = 1 - 2^-3.
    const double mid =
        shedProbability(rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1));
    const double high =
        shedProbability(rssAtPercent(95), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1));
    ASSERT_APPROX_EQUAL(high, 1.0 - std::pow(2.0, -3.0), 1e-6);
    ASSERT_LT(mid, high);
}

TEST(QueryMemoryLoadSheddingProbability, ShorterIntervalYieldsSmallerProbability) {
    const double full =
        shedProbability(rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1));
    const double half = shedProbability(
        rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Milliseconds(500));
    ASSERT_LT(half, full);
    ASSERT_APPROX_EQUAL(half, 1.0 - std::pow(2.0, -0.5), 1e-6);
}

TEST(QueryMemoryLoadSheddingProbability, DegenerateInputsReturnZero) {
    // No RSS sample yet.
    ASSERT_EQ(0.0, shedProbability(-1, kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1)));
    // Non-positive memory limit.
    ASSERT_EQ(0.0,
              shedProbability(rssAtPercent(90), 0, kSizeRef, kLow, kHigh, kSizeRef, Seconds(1)));
    // High mark not above low mark.
    ASSERT_EQ(0.0,
              shedProbability(rssAtPercent(90), kMemLimit, kSizeRef, 90, 90, kSizeRef, Seconds(1)));
    // Zero elapsed time (between the marks) -> zero probability.
    ASSERT_EQ(0.0,
              shedProbability(
                  rssAtPercent(90), kMemLimit, kSizeRef, kLow, kHigh, kSizeRef, Milliseconds(0)));
}

// Restores both water-mark knobs on scope exit so validation tests are order-independent.
auto scopedRestoreMarks() {
    const std::int32_t savedLow = gQueryMemoryLoadSheddingLowMarkPercent.load();
    const std::int32_t savedHigh = gQueryMemoryLoadSheddingHighMarkPercent.load();
    return ScopeGuard([savedLow, savedHigh] {
        gQueryMemoryLoadSheddingLowMarkPercent.store(savedLow);
        gQueryMemoryLoadSheddingHighMarkPercent.store(savedHigh);
    });
}

TEST(QueryMemoryLoadSheddingValidation, LowMarkMustStayBelowHighMark) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingHighMarkPercent.store(85);

    // -1 disables the feature: no relationship is enforced.
    ASSERT_OK(validateQueryMemoryLoadSheddingLowMark(-1, boost::none));
    // Enabled and strictly below the high mark: accepted.
    ASSERT_OK(validateQueryMemoryLoadSheddingLowMark(70, boost::none));
    // Enabled and at/above the high mark: rejected, since it would silently disarm shedding.
    ASSERT_NOT_OK(validateQueryMemoryLoadSheddingLowMark(85, boost::none));
    ASSERT_NOT_OK(validateQueryMemoryLoadSheddingLowMark(90, boost::none));
}

TEST(QueryMemoryLoadSheddingValidation, HighMarkMustStayAboveLowMark) {
    auto restore = scopedRestoreMarks();

    // Feature disabled (low = -1): any legal high mark is accepted.
    gQueryMemoryLoadSheddingLowMarkPercent.store(-1);
    ASSERT_OK(validateQueryMemoryLoadSheddingHighMark(10, boost::none));

    // Feature enabled (low = 70): the high mark must stay strictly above the low mark.
    gQueryMemoryLoadSheddingLowMarkPercent.store(70);
    ASSERT_OK(validateQueryMemoryLoadSheddingHighMark(85, boost::none));
    ASSERT_NOT_OK(validateQueryMemoryLoadSheddingHighMark(70, boost::none));
    ASSERT_NOT_OK(validateQueryMemoryLoadSheddingHighMark(60, boost::none));
}

// Drives queryMemoryCheckLoadShedding() end-to-end against an operation with a memory tracker,
// using the test-only pressure-override failpoint to place RSS below or above the low mark.
class QueryMemoryLoadSheddingCheckTest : public AggregationContextFixture {};

void setPressureOverride(int usagePercent) {
    globalFailPointRegistry()
        .find("queryMemoryPressureOverride")
        ->setMode(FailPoint::alwaysOn, 0, BSON("usagePercent" << usagePercent));
}

// Below the low mark the fast path marks the dt baseline invalid (Date_t{}), and the first
// check after RSS crosses above the mark primes the baseline to "now" without shedding -- so the
// zero-hazard time spent below the mark is never credited as dt on the crossing. Without the
// priming branch the baseline would stay at its initial value and each assertion below would fail.
TEST_F(QueryMemoryLoadSheddingCheckTest, FastPathPrimesFirstCheckAfterCrossingLowMark) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(
        50);  // enable load shedding (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);  // opt-in: stand in for a user-facing read
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    auto* tracker = owned.get();
    tracker->add(1 << 20);  // tracked memory so the op is a real shedding candidate
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    // Below the low mark: not shed, and the baseline is marked invalid.
    setPressureOverride(10);
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));
    ASSERT_EQ(query_memory_load_shedding_detail::lastEvalTimeForTest(opCtx), Date_t{});

    // Cross above the mark. The first check must not shed; it re-establishes the baseline at "now"
    // instead of crediting the below-mark interval as dt. That the baseline is no longer the
    // invalid sentinel proves the priming branch ran (and thus returned OK for that reason).
    setPressureOverride(90);
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));
    ASSERT_NE(query_memory_load_shedding_detail::lastEvalTimeForTest(opCtx), Date_t{});
}

// The test-only always-shed failpoint must be deterministic for any eligible (enabled,
// memory-tracked, non-exempt, over-the-low-mark) operation, bypassing the dt-based priming and
// throttle gates -- otherwise a fast op whose checks land during priming or within the throttle
// window escapes shedding, making failpoint-driven tests flaky.
TEST_F(QueryMemoryLoadSheddingCheckTest, AlwaysShedFailpointBypassesTimingGates) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);  // opt-in: stand in for a user-facing read
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    owned->add(1 << 20);  // tracked memory so the op is a real shedding candidate
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    // Arm the invalid-baseline sentinel via a below-mark check, so the next above-mark check would
    // otherwise be the non-shedding priming check.
    setPressureOverride(10);
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));

    // Cross above the mark with the failpoint on: the op is shed on this very first check despite
    // the primed baseline, and the kill is made sticky via markKilled.
    setPressureOverride(90);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);
    ASSERT_EQ(queryMemoryCheckLoadShedding(opCtx), ErrorCodes::QueryMemoryLimitExceeded);
    ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::QueryMemoryLimitExceeded);
}

// An operation with no tracked memory has zero shed probability and must never be shed -- not even
// when the always-shed failpoint is on. This mirrors a cursor that has already spilled its memory:
// a releaseMemory spill leaves ~0 tracked bytes, and the following getMore must not be shed.
TEST_F(QueryMemoryLoadSheddingCheckTest, ZeroTrackedMemoryIsNeverShedEvenWhenForced) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);  // opt-in: stand in for a user-facing read
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx,
                                                          std::move(owned));  // 0 tracked bytes

    // Well above the mark, with the failpoint forcing a shed: still not shed, because the operation
    // consumes no tracked memory.
    setPressureOverride(95);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));
    ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::OK);
}

// A shed increments the exported OTel operationsShed counter (the fleet-monitoring mirror of the
// in-process atomic).
TEST_F(QueryMemoryLoadSheddingCheckTest, ShedIncrementsOtelOperationsShedCounter) {
    // The capturer must exist before the counter is created/recorded; constructing it also
    // reinitializes MetricsService and resets recorded values, isolating this case.
    otel::metrics::OtelMetricsCapturer capturer;

    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);  // opt-in: stand in for a user-facing read
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    owned->add(1 << 20);  // tracked memory so the op is a real shedding candidate
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    setPressureOverride(90);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);
    ASSERT_EQ(queryMemoryCheckLoadShedding(opCtx), ErrorCodes::QueryMemoryLimitExceeded);

    // Not all platforms can read OTel metrics back in-process (e.g. Windows); skip the assertion
    // there.
    if (capturer.canReadMetrics()) {
        ASSERT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kQueryMemoryLoadSheddingOperationsShed),
                  1);
    }
}

// An operation that has taken a write-intent (IX/X) global lock is never shed, even when forced:
// the Locker's write-lock flag latches, so we never abort after a partial, non-idempotent write.
// This is what protects a $merge/$out write inside an eligible aggregate.
TEST_F(QueryMemoryLoadSheddingCheckTest, WriteIntentLockedOperationIsNeverShed) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);  // opt-in: stand in for a user-facing read
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    owned->add(1 << 20);  // tracked memory so the op is a real shedding candidate
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    setPressureOverride(90);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);

    shard_role_details::getLocker(opCtx)->setGlobalLockTakenInMode(MODE_IX);
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));
    ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::OK);
}

// A read (and its disk spills) holds only a shared/intent-shared global lock, never a write-intent
// one -- SpillTable uses its own RecoveryUnit and takes no global write lock. So an IS-locked
// operation is still shed: disk spilling does not disable load shedding.
TEST_F(QueryMemoryLoadSheddingCheckTest, ReadIntentLockedOperationIsStillShed) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    OperationContext* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    owned->add(1 << 20);
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    setPressureOverride(90);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);

    shard_role_details::getLocker(opCtx)->setGlobalLockTakenInMode(MODE_IS);
    ASSERT_EQ(queryMemoryCheckLoadShedding(opCtx), ErrorCodes::QueryMemoryLimitExceeded);
}

// Opt-in: an operation that no user-facing read command marked eligible is never shed, even when a
// shed is forced. Marking it eligible (as find/aggregate/getMore do) makes it a candidate.
TEST_F(QueryMemoryLoadSheddingCheckTest, IneligibleOperationIsNeverShed) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);  // enable (high mark stays default)
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        globalFailPointRegistry()
            .find("queryMemoryLoadSheddingAlwaysShed")
            ->setMode(FailPoint::off);
    });

    auto* opCtx = getExpCtx()->getOperationContext();  // deliberately left ineligible
    auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
    owned->add(1 << 20);  // tracked memory: a candidate but for eligibility
    OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));

    setPressureOverride(95);
    globalFailPointRegistry()
        .find("queryMemoryLoadSheddingAlwaysShed")
        ->setMode(FailPoint::alwaysOn);

    // Not eligible (no read command opted in): never shed, even forced.
    ASSERT_OK(queryMemoryCheckLoadShedding(opCtx));
    ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::OK);

    // Opt in, as a user-facing read command does: now it is shed.
    markOperationQueryMemorySheddingEligible(opCtx);
    ASSERT_EQ(queryMemoryCheckLoadShedding(opCtx), ErrorCodes::QueryMemoryLimitExceeded);
}

using QueryMemoryLoadSheddingEligibility = AggregationContextFixture;

// Eligibility defaults false and flips to true once a read command marks it.
TEST_F(QueryMemoryLoadSheddingEligibility, DefaultsFalseAndMarks) {
    auto* opCtx = getExpCtx()->getOperationContext();
    ASSERT_FALSE(isOperationQueryMemorySheddingEligible(opCtx));
    markOperationQueryMemorySheddingEligible(opCtx);
    ASSERT_TRUE(isOperationQueryMemorySheddingEligible(opCtx));
}

// Eligibility is scoped to the query's QueryLifespan, so it carries onto a later opCtx bound to the
// same lifespan (the getMore case) but not onto an unrelated opCtx that has its own lifespan.
TEST_F(QueryMemoryLoadSheddingEligibility, PersistsAcrossBoundLifespanButNotSeparateOne) {
    auto* opCtx = getExpCtx()->getOperationContext();
    markOperationQueryMemorySheddingEligible(opCtx);
    auto handle = QueryLifespan::get(opCtx).handle();

    auto client = getServiceContext()->getService()->makeClient("getmore-op");
    auto laterOpCtx = client->makeOperationContext();

    // Unbound: the later opCtx has its own lifespan and starts ineligible.
    ASSERT_FALSE(isOperationQueryMemorySheddingEligible(laterOpCtx.get()));

    // Bound to the originating query's lifespan, as a getMore does: eligibility carries over.
    QueryLifespan::AlternativeQueryRegion region(laterOpCtx.get(), handle);
    ASSERT_TRUE(isOperationQueryMemorySheddingEligible(laterOpCtx.get()));
}

// ---------------------------------------------------------------------------------------------
// Queued-operation sweep: operations blocked waiting for an execution ticket cannot reach a query
// checkpoint, so the RSS monitor evaluates them. These tests install a one-ticket TicketingSystem,
// park a second operation in the ticket queue on its own thread, and drive the sweep directly.
// ---------------------------------------------------------------------------------------------

class QueryMemoryLoadSheddingQueuedSweepTest : public ClockSourceMockServiceContextTest {
public:
    using TicketHolder = mongo::TicketHolder;
    using TicketingSystem = admission::execution_control::TicketingSystem;

    void setUp() override {
        ClockSourceMockServiceContextTest::setUp();
        auto* svcCtx = getServiceContext();
        auto makeHolder = [&](int n) {
            return std::make_unique<TicketHolder>(svcCtx,
                                                  n,
                                                  false /*trackPeakUsed*/,
                                                  TicketHolder::kDefaultMaxQueueDepth,
                                                  nullptr /*delinquentCallback*/,
                                                  nullptr /*acquisitionCallback*/,
                                                  nullptr /*waitedAcquisitionCallback*/,
                                                  nullptr /*releaseCallback*/,
                                                  nullptr /*startQueueingCallback*/,
                                                  TicketHolder::ResizePolicy::kGradual,
                                                  semaphoreType());
        };
        TicketingSystem::use(
            svcCtx,
            std::make_unique<TicketingSystem>(
                svcCtx,
                TicketingSystem::RWTicketHolder{makeHolder(1), makeHolder(1)},
                TicketingSystem::RWTicketHolder{makeHolder(1), makeHolder(1)},
                admission::execution_control::ExecutionControlConcurrencyAdjustmentAlgorithmEnum::
                    kFixedConcurrentTransactions));
    }

    // The semaphore backing every ticket pool. The default matches the normal-priority pools.
    virtual TicketHolder::SemaphoreType semaphoreType() const {
        return TicketHolder::SemaphoreType::kCompeting;
    }

    void tearDown() override {
        TicketingSystem::use(getServiceContext(), nullptr);
        ClockSourceMockServiceContextTest::tearDown();
    }

    struct ClientAndOp {
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;
    };

    ClientAndOp makeClientAndOp(std::string_view name) {
        ClientAndOp c;
        c.client = getService()->makeClient(std::string{name});
        c.opCtx = c.client->makeOperationContext();
        return c;
    }

    // Makes 'opCtx' look like a memory-tracked, shed-eligible read holding 'bytes' of tracked
    // memory, published to CurOp the way a real stage tracker does.
    static void makeEligibleHolding(OperationContext* opCtx, int64_t bytes) {
        markOperationQueryMemorySheddingEligible(opCtx);
        auto owned = std::make_unique<OperationMemoryUsageTracker>(opCtx);
        owned->add(bytes);
        OperationMemoryUsageTracker::attachToOpCtxIfAvailable(opCtx, std::move(owned));
        CurOp::get(opCtx)->setMemoryTrackingStats(bytes, bytes);
    }

    static void waitUntilQueued(
        OperationContext* opCtx,
        Locker::ClientState queuedState = Locker::ClientState::kQueuedReader) {
        const auto* locker = shard_role_details::getLocker(opCtx);
        const Date_t deadline = Date_t::now() + Seconds(30);
        while (locker->getClientState() != queuedState) {
            ASSERT_LT(Date_t::now(), deadline) << "operation never queued for a ticket";
            sleepmillis(1);
        }
    }

    PseudoRandom& prng() {
        return getClient()->getPrng();
    }

    // Advances the mock fast clock the sweep reads, past the per-evaluation throttle.
    void advanceClock() {
        static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource())
            ->advance(Milliseconds(2));
    }

    // End to end: a shed-eligible read blocked on the (single, taken) read ticket is killed by the
    // sweep with QueryMemoryLimitExceeded, wakes from its ticket wait with that error, and the
    // bookkeeping (operationsShed) records it. The running ticket holder is untouched.
    void runKillsEligibleOperationBlockedOnTicket() {
        auto restore = scopedRestoreMarks();
        gQueryMemoryLoadSheddingLowMarkPercent.store(50);
        gQueryMemoryLoadSheddingHighMarkPercent.store(60);
        ON_BLOCK_EXIT([] {
            globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
        });
        // Above the high mark: p == 1, so the sweep is deterministic once the clock is primed.
        setPressureOverride(90);

        auto holder = makeClientAndOp("ticket holder");
        makeEligibleHolding(holder.opCtx.get(), 1 << 20);
        Lock::GlobalRead holderLock(holder.opCtx.get());  // takes the only read ticket

        auto waiter = makeClientAndOp("ticket waiter");
        OperationContext* waiterOpCtx = waiter.opCtx.get();
        makeEligibleHolding(waiterOpCtx, 1 << 20);

        Status waiterResult = Status::OK();
        stdx::thread waiterThread([&] {
            try {
                Lock::GlobalRead lk(waiterOpCtx);  // blocks: no ticket available
            } catch (const DBException& ex) {
                waiterResult = ex.toStatus();
            }
        });
        // If an assertion fails before the shed, wake the still-queued waiter so the join can't
        // hang.
        ScopeGuard join([&] {
            waiterOpCtx->markKilled(ErrorCodes::Interrupted);
            waiterThread.join();
        });
        waitUntilQueued(waiterOpCtx);

        const long long shedBefore = [&] {
            BSONObjBuilder b;
            appendQueryMemoryLoadSheddingStats(getServiceContext(), b);
            return b.obj()["loadShedding"]["operationsShed"].numberLong();
        }();

        // First pass only primes the sweep clock for the newly observed queued op.
        auto candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
        ASSERT_EQ(candidates, 1);
        ASSERT_EQ(waiterOpCtx->getKillStatus(), ErrorCodes::OK);

        // Second pass, at least the throttle interval later, sheds it (p == 1 above the high mark).
        advanceClock();
        candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
        ASSERT_EQ(candidates, 1);
        ASSERT_EQ(waiterOpCtx->getKillStatus(), ErrorCodes::QueryMemoryLimitExceeded);
        // The running ticket holder was never a candidate.
        ASSERT_EQ(holder.opCtx->getKillStatus(), ErrorCodes::OK);

        // The kill wakes the waiter out of its ticket wait without it ever being admitted.
        join.dismiss();
        waiterThread.join();
        ASSERT_EQ(waiterResult, ErrorCodes::QueryMemoryLimitExceeded);

        BSONObjBuilder b;
        appendQueryMemoryLoadSheddingStats(getServiceContext(), b);
        // Hold the builder's obj so 'after' does not view a buffer owned by a temporary.
        BSONObj statsObj = b.obj();
        const BSONObj after = statsObj["loadShedding"].Obj();
        // The queued sweep's shed is counted in operationsShed like any other shed.
        ASSERT_EQ(after["operationsShed"].numberLong(), shedBefore + 1);
    }
};

// The normal-priority pools use the competing semaphore, which also wakes waiters periodically.
TEST_F(QueryMemoryLoadSheddingQueuedSweepTest, KillsEligibleOperationBlockedOnTicket) {
    runKillsEligibleOperationBlockedOnTicket();
}

// Execution control deprioritizes long-running queries into low-priority pools backed by the
// ordered semaphore, which never wakes a waiter periodically: the sweep's kill must wake it.
class QueryMemoryLoadSheddingQueuedSweepOrderedTest
    : public QueryMemoryLoadSheddingQueuedSweepTest {
    TicketHolder::SemaphoreType semaphoreType() const override {
        return TicketHolder::SemaphoreType::kPrioritizeFewestAdmissions;
    }
};

TEST_F(QueryMemoryLoadSheddingQueuedSweepOrderedTest, KillsEligibleOperationBlockedOnTicket) {
    runKillsEligibleOperationBlockedOnTicket();
}

// The sweep is a no-op when the feature is off or RSS is below the low mark -- even with an
// eligible operation parked in the queue across many passes -- and sheds that same operation once
// RSS rises above the low mark, so the earlier no-ops are due to the low-mark gate.
TEST_F(QueryMemoryLoadSheddingQueuedSweepTest, ShedsQueuedOperationOnlyAboveLowMark) {
    auto restore = scopedRestoreMarks();
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
    });

    auto holder = makeClientAndOp("ticket holder");
    Lock::GlobalRead holderLock(holder.opCtx.get());
    auto waiter = makeClientAndOp("ticket waiter");
    OperationContext* waiterOpCtx = waiter.opCtx.get();
    makeEligibleHolding(waiterOpCtx, 1 << 20);
    Status waiterResult = Status::OK();
    stdx::thread waiterThread([&] {
        try {
            Lock::GlobalRead lk(waiterOpCtx);
        } catch (const DBException& ex) {
            waiterResult = ex.toStatus();
        }
    });
    ScopeGuard cleanup([&] {
        waiterOpCtx->markKilled(ErrorCodes::Interrupted);
        waiterThread.join();
    });
    waitUntilQueued(waiterOpCtx);

    // Feature disabled.
    gQueryMemoryLoadSheddingLowMarkPercent.store(-1);
    setPressureOverride(99);
    auto candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
    ASSERT_EQ(candidates, 0);

    // Enabled but below the low mark: never a candidate. Two passes would prime and then charge the
    // op's clock if the low-mark gate were missing.
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);
    gQueryMemoryLoadSheddingHighMarkPercent.store(60);
    setPressureOverride(40);
    for (int pass = 0; pass < 2; ++pass) {
        advanceClock();
        candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
        ASSERT_EQ(candidates, 0) << "pass " << pass;
    }
    ASSERT_EQ(waiterOpCtx->getKillStatus(), ErrorCodes::OK);

    // Above the high mark (p == 1), the same op is shed: the first pass primes its clock and the
    // second sheds it.
    setPressureOverride(90);
    candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
    ASSERT_EQ(candidates, 1);
    advanceClock();
    candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
    ASSERT_EQ(candidates, 1);
    ASSERT_EQ(waiterOpCtx->getKillStatus(), ErrorCodes::QueryMemoryLimitExceeded);
    cleanup.dismiss();
    waiterThread.join();
    ASSERT_EQ(waiterResult, ErrorCodes::QueryMemoryLimitExceeded);
}

// Candidacy requires all of: queued for a read ticket, shed-eligible, no write-intent lock, tracked
// memory > 0. Each missing condition alone excludes the operation.
TEST_F(QueryMemoryLoadSheddingQueuedSweepTest, SkipsIneligibleQueuedOperations) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);
    gQueryMemoryLoadSheddingHighMarkPercent.store(60);
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
    });
    setPressureOverride(99);

    auto holder = makeClientAndOp("ticket holder");
    makeEligibleHolding(holder.opCtx.get(), 1 << 20);  // eligible but RUNNING: never a candidate
    Lock::GlobalRead holderLock(holder.opCtx.get());

    // Not eligible (never opted in) but holding tracked memory.
    auto notEligible = makeClientAndOp("not eligible");
    {
        auto owned = std::make_unique<OperationMemoryUsageTracker>(notEligible.opCtx.get());
        owned->add(1 << 20);
        OperationMemoryUsageTracker::attachToOpCtxIfAvailable(notEligible.opCtx.get(),
                                                              std::move(owned));
        CurOp::get(notEligible.opCtx.get())->setMemoryTrackingStats(1 << 20, 1 << 20);
    }

    // Eligible, but no tracked memory published.
    auto noMemory = makeClientAndOp("no memory");
    markOperationQueryMemorySheddingEligible(noMemory.opCtx.get());

    // Eligible with memory, but has taken a write-intent global lock (exempt).
    auto writer = makeClientAndOp("writer");
    makeEligibleHolding(writer.opCtx.get(), 1 << 20);
    shard_role_details::getLocker(writer.opCtx.get())->setGlobalLockTakenInMode(MODE_IX);

    std::vector<OperationContext*> waiters{
        notEligible.opCtx.get(), noMemory.opCtx.get(), writer.opCtx.get()};
    std::vector<stdx::thread> threads;
    for (auto* opCtx : waiters) {
        threads.emplace_back([opCtx] {
            try {
                Lock::GlobalRead lk(opCtx);
            } catch (const DBException&) {
            }
        });
    }
    ScopeGuard cleanup([&] {
        for (auto* opCtx : waiters) {
            opCtx->markKilled(ErrorCodes::Interrupted);
        }
        for (auto& t : threads) {
            t.join();
        }
    });
    for (auto* opCtx : waiters) {
        waitUntilQueued(opCtx);
    }

    for (int pass = 0; pass < 2; ++pass) {
        advanceClock();
        auto candidates = sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng());
        ASSERT_EQ(candidates, 0) << "pass " << pass;
    }
    for (auto* opCtx : waiters) {
        ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::OK);
    }
    ASSERT_EQ(holder.opCtx->getKillStatus(), ErrorCodes::OK);
}

// An eligible operation queued for a write ticket is never a candidate: once granted, it could
// begin writing before a kill is observed.
TEST_F(QueryMemoryLoadSheddingQueuedSweepTest, SkipsQueuedWriters) {
    auto restore = scopedRestoreMarks();
    gQueryMemoryLoadSheddingLowMarkPercent.store(50);
    gQueryMemoryLoadSheddingHighMarkPercent.store(60);
    ON_BLOCK_EXIT([] {
        globalFailPointRegistry().find("queryMemoryPressureOverride")->setMode(FailPoint::off);
    });
    setPressureOverride(99);

    auto holder = makeClientAndOp("write ticket holder");
    // Takes the only write ticket.
    Lock::GlobalLock holderLock(holder.opCtx.get(), MODE_IX);

    auto writer = makeClientAndOp("queued writer");
    makeEligibleHolding(writer.opCtx.get(), 1 << 20);
    stdx::thread thread([opCtx = writer.opCtx.get()] {
        try {
            Lock::GlobalLock lk(opCtx, MODE_IX);
        } catch (const DBException&) {
        }
    });
    ScopeGuard cleanup([&] {
        writer.opCtx->markKilled(ErrorCodes::Interrupted);
        thread.join();
    });
    waitUntilQueued(writer.opCtx.get(), Locker::ClientState::kQueuedWriter);

    for (int pass = 0; pass < 2; ++pass) {
        advanceClock();
        ASSERT_EQ(sweepQueuedOperationsForMemoryPressure(getServiceContext(), prng()), 0)
            << "pass " << pass;
    }
    ASSERT_EQ(writer.opCtx->getKillStatus(), ErrorCodes::OK);
}

}  // namespace
}  // namespace mongo
