// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/throughput_probing.h"

#include "mongo/db/admission/execution_control/throughput_probing_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mock_periodic_runner.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo::admission::execution_control::throughput_probing {

TEST(ThroughputProbingParameterTest, InitialConcurrency) {
    // The validator intentionally performs no validation and always returns OK. Validation is
    // deferred to _initState() to avoid startup failures due to parameter ordering issues.
    // All values should pass validation.
    ASSERT_OK(validateInitialConcurrency(0, {}));
    ASSERT_OK(validateInitialConcurrency(gMinConcurrency.load() * 2, {}));
    ASSERT_OK(validateInitialConcurrency(gMaxConcurrency.load() * 2, {}));
    ASSERT_OK(validateInitialConcurrency(1, {}));                           // Below minimum
    ASSERT_OK(validateInitialConcurrency(gMaxConcurrency.load() * 3, {}));  // Above maximum
}

class InitStateWarningTest : public ServiceContextTest {
protected:
    std::unique_ptr<ScopedGlobalServiceContextForTest> makeScopedServiceContext() override {
        return std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
            nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()));
    }

    void setUp() override {
        ServiceContextTest::setUp();
        _readTicketHolder.emplace(
            getServiceContext(), 0, /*trackPeakUsed=*/true, TicketHolder::kDefaultMaxQueueDepth);
        _writeTicketHolder.emplace(
            getServiceContext(), 0, /*trackPeakUsed=*/true, TicketHolder::kDefaultMaxQueueDepth);
        getServiceContext()->setPeriodicRunner(std::make_unique<MockPeriodicRunner>());
    }

    void createAndInitProbing(int32_t initialConcurrency) {
        gInitialConcurrency = initialConcurrency;
        throughput_probing::gConcurrencyAdjustmentIntervalMillis.store(1);
        _throughputProbing = std::make_unique<ThroughputProbing>(
            getServiceContext(), &readTicketHolder(), &writeTicketHolder());
        throughputProbing()._initState();
    }

private:
    TicketHolder& readTicketHolder() {
        return *_readTicketHolder;
    }

    TicketHolder& writeTicketHolder() {
        return *_writeTicketHolder;
    }

    ThroughputProbing& throughputProbing() {
        return *_throughputProbing;
    }

    boost::optional<TicketHolder> _readTicketHolder;
    boost::optional<TicketHolder> _writeTicketHolder;
    std::unique_ptr<ThroughputProbing> _throughputProbing;
};

TEST_F(InitStateWarningTest, NoWarningForDefaultValue) {
    // Default value of 0 should not produce any warnings.
    unittest::LogCaptureGuard logs;
    createAndInitProbing(0);
    logs.stop();

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352800)), 0);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352801)), 0);
}

TEST_F(InitStateWarningTest, NoWarningForValidRange) {
    // Values within [2 * minConcurrency, 2 * maxConcurrency] should not produce warnings.
    unittest::LogCaptureGuard logs;
    createAndInitProbing(gMinConcurrency.load() * 2);
    logs.stop();

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352800)), 0);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352801)), 0);
}

TEST_F(InitStateWarningTest, WarningForBelowMinimum) {
    // Values below 2 * minConcurrency should log warning 11352800.
    int32_t belowMin = gMinConcurrency.load();  // This is < 2 * gMinConcurrency
    unittest::LogCaptureGuard logs;
    createAndInitProbing(belowMin);
    logs.stop();

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352800)), 1);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352801)), 0);

    // Verify the warning contains the expected attributes.
    ASSERT_EQ(
        logs.countBSONContainingSubset(BSON(
            "id" << 11352800 << "attr"
                 << BSON("configured" << belowMin << "minimum" << gMinConcurrency.load() * 2))),
        1);
}

TEST_F(InitStateWarningTest, WarningForAboveMaximum) {
    // Values above 2 * maxConcurrency should log warning 11352801.
    int32_t aboveMax = gMaxConcurrency.load() * 2 + 1;
    unittest::LogCaptureGuard logs;
    createAndInitProbing(aboveMax);
    logs.stop();

    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352800)), 0);
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 11352801)), 1);

    // Verify the warning contains the expected attributes.
    ASSERT_EQ(
        logs.countBSONContainingSubset(BSON(
            "id" << 11352801 << "attr"
                 << BSON("configured" << aboveMax << "maximum" << gMaxConcurrency.load() * 2))),
        1);
}

TEST(ThroughputProbingParameterTest, MinConcurrency) {
    ASSERT_OK(validateMinConcurrency(5, {}));
    ASSERT_OK(validateMinConcurrency(gMaxConcurrency.load(), {}));
    ASSERT_NOT_OK(validateMinConcurrency(0, {}));
    ASSERT_NOT_OK(validateMinConcurrency(gMaxConcurrency.load() + 1, {}));
}

TEST(ThroughputProbingParameterTest, MaxConcurrency) {
    ASSERT_OK(validateMaxConcurrency(gMinConcurrency.load(), {}));
    ASSERT_OK(validateMaxConcurrency(256, {}));
    ASSERT_NOT_OK(validateMaxConcurrency(gMinConcurrency.load() - 1, {}));
}

class ThroughputProbingTest : public ServiceContextTest {
protected:
    std::unique_ptr<ScopedGlobalServiceContextForTest> makeScopedServiceContext() override {
        return std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
            nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()));
    }

    void setUp() override {
        ServiceContextTest::setUp();
        _svcCtx = getServiceContext();
        _readTicketHolder.emplace(
            _svcCtx, 0, true /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);
        _writeTicketHolder.emplace(
            _svcCtx, 0, true /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth);

        // The ThroughputProbing constructor requires the periodic runner to be set up on _svcCtx.
        _svcCtx->setPeriodicRunner(std::make_unique<MockPeriodicRunner>());
        throughput_probing::gInitialConcurrency = size();
        throughput_probing::gReadWriteRatio.store(readWriteRatio());
        throughput_probing::gConcurrencyAdjustmentIntervalMillis.store(1);
        _throughputProbing =
            std::make_unique<ThroughputProbing>(_svcCtx, &readTicketHolder(), &writeTicketHolder());

        {
            auto client = _svcCtx->getService()->makeClient("ThroughputProbingInit");
            auto opCtx = client->makeOperationContext();
            throughputProbing()._job =
                _svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
                    "ThroughputProbingTicketHolderMonitor",
                    [this](Client* client) { throughputProbing()._run(client); },
                    Milliseconds{throughput_probing::gConcurrencyAdjustmentIntervalMillis.load()},
                    true /* isKillableByStepdown */});
            throughputProbing()._initState();
            throughputProbing()._resetConcurrency(opCtx.get());
        }

        // We need to advance the ticks to something other than zero, since that is used to
        // determine the if we're in the first iteration or not.
        _tick();

        // First loop is a no-op and initializes state.
        _run();
    }

    virtual int32_t size() const {
        return 64;
    }

    virtual double readWriteRatio() const {
        return 0.5;
    }

    void _run() {
        _runner()->run(getClient());
        _statsTester.set(throughputProbing());
    }

    void _tick() {
        _tickSource()->advance(Microseconds(1000));
    }

    TickSourceMock<Microseconds>* _tickSource() {
        return checked_cast<decltype(_tickSource())>(getServiceContext()->getTickSource());
    }

    MockPeriodicRunner* _runner() {
        return checked_cast<decltype(_runner())>(getServiceContext()->getPeriodicRunner());
    }

    TicketHolder& readTicketHolder() {
        return *_readTicketHolder;
    }

    TicketHolder& writeTicketHolder() {
        return *_writeTicketHolder;
    }

    ThroughputProbing& throughputProbing() {
        return *_throughputProbing;
    }

    // Built in setUp(): default member initializers run before the ServiceContext exists on
    // the deferred-setup path.
    ServiceContext* _svcCtx = nullptr;
    boost::optional<TicketHolder> _readTicketHolder;
    boost::optional<TicketHolder> _writeTicketHolder;
    std::unique_ptr<ThroughputProbing> _throughputProbing;

    class StatsTester {
    public:
        void set(ThroughputProbing& throughputProbing) {
            BSONObjBuilder stats;
            throughputProbing.appendStats(stats);
            _prevStats = std::exchange(_stats, stats.obj());
        }

        bool concurrencyIncreased() const {
            return _stats["timesIncreased"].Long() == _prevStats["timesIncreased"].Long() + 1 &&
                _stats["totalAmountIncreased"].Long() > _prevStats["totalAmountIncreased"].Long() &&
                _stats["timesDecreased"].Long() == _prevStats["timesDecreased"].Long() &&
                _stats["totalAmountDecreased"].Long() == _prevStats["totalAmountDecreased"].Long();
        }

        bool concurrencyDecreased() const {
            return _stats["timesDecreased"].Long() == _prevStats["timesDecreased"].Long() + 1 &&
                _stats["totalAmountDecreased"].Long() > _prevStats["totalAmountDecreased"].Long() &&
                _stats["timesIncreased"].Long() == _prevStats["timesIncreased"].Long() &&
                _stats["totalAmountIncreased"].Long() == _prevStats["totalAmountIncreased"].Long();
        }

        bool concurrencyKept() const {
            return !concurrencyIncreased() && !concurrencyDecreased();
        }

        bool probeIncemented(std::string a) const {
            return _stats[a].Long() > _prevStats[a].Long();
        }

        std::string toString() const {
            return str::stream() << "Stats: " << _stats << ", previous stats: " << _prevStats;
        }

    private:
        BSONObj _stats =
            BSON("timesDecreased" << 0ll << "timesIncreased" << 0ll << "totalAmountDecreased" << 0ll
                                  << "totalAmountIncreased" << 0ll << "timesProbedStable" << 0ll
                                  << "timesProbedUp" << 0ll << "timesProbedDown" << 0ll);
        BSONObj _prevStats =
            BSON("timesDecreased" << 0ll << "timesIncreased" << 0ll << "totalAmountDecreased" << 0ll
                                  << "totalAmountIncreased" << 0ll << "timesProbedStable" << 0ll
                                  << "timesProbedUp" << 0ll << "timesProbedDown" << 0ll);
    } _statsTester;
};

class ThroughputProbingMaxConcurrencyTest : public ThroughputProbingTest {
protected:
    // This input is the total initial concurrency between both ticketholders, so it will be split
    // evenly between each ticketholder. We are attempting to test a limit that is per-ticketholder.
    int32_t size() const override {
        return gMaxConcurrency.load() * 2;
    }
};

class ThroughputProbingMinConcurrencyTest : public ThroughputProbingTest {
protected:
    // This input is the total initial concurrency between both ticketholders, so it will be split
    // evenly between each ticketholder. We are attempting to test a limit that is per-ticketholder.
    int32_t size() const override {
        return gMinConcurrency.load() * 2;
    }
};

class ThroughputProbingReadHeavyTest : public ThroughputProbingTest {
protected:
    int32_t size() const override {
        return 16;
    }
    double readWriteRatio() const override {
        return 0.9;
    }
};

class ThroughputProbingWriteHeavyTest : public ThroughputProbingTest {
protected:
    int32_t size() const override {
        return 16;
    }
    double readWriteRatio() const override {
        return 0.1;
    }
};

TEST_F(ThroughputProbingTest, ProbeUpSucceeds) {
    // Tickets are exhausted.
    auto initialSize = readTicketHolder().outof();
    auto size = initialSize;
    readTicketHolder().setPeakUsed_forTest(size);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(readTicketHolder().outof(), size);
    ASSERT_GT(writeTicketHolder().outof(), size);

    // Throughput inreases.
    size = readTicketHolder().outof();
    readTicketHolder().setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(readTicketHolder().outof(), size);
    ASSERT_GT(readTicketHolder().outof(), initialSize);
    ASSERT_LT(writeTicketHolder().outof(), size);
    ASSERT_GT(writeTicketHolder().outof(), initialSize);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeUpFails) {
    // Tickets are exhausted.
    auto size = readTicketHolder().outof();
    readTicketHolder().setPeakUsed_forTest(size);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(readTicketHolder().outof(), size);
    ASSERT_GT(writeTicketHolder().outof(), size);

    // Throughput does not increase.
    readTicketHolder().setNumFinishedProcessing_forTest(2);
    _tick();

    // Probing up fails since throughput did not increase. Return to stable.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size);
    ASSERT_EQ(writeTicketHolder().outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeDownSucceeds) {
    // Tickets are not exhausted.
    auto initialSize = readTicketHolder().outof();
    auto size = initialSize;
    readTicketHolder().setPeakUsed_forTest(size - 1);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(readTicketHolder().outof(), size);
    ASSERT_LT(writeTicketHolder().outof(), size);

    // Throughput increases.
    size = readTicketHolder().outof();
    readTicketHolder().setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(readTicketHolder().outof(), initialSize);
    ASSERT_GT(readTicketHolder().outof(), size);
    ASSERT_LT(writeTicketHolder().outof(), initialSize);
    ASSERT_GT(writeTicketHolder().outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyDecreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeDownFails) {
    // Tickets are not exhausted.
    auto size = readTicketHolder().outof();
    readTicketHolder().setPeakUsed_forTest(size - 1);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(readTicketHolder().outof(), size);
    ASSERT_LT(writeTicketHolder().outof(), size);

    // Throughput does not increase.
    readTicketHolder().setNumFinishedProcessing_forTest(2);
    _tick();

    // Probing down fails since throughput did not increase. Return back to stable.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size);
    ASSERT_EQ(writeTicketHolder().outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();
}

TEST_F(ThroughputProbingMaxConcurrencyTest, NoProbeUp) {
    // Tickets are exhausted.
    auto size = readTicketHolder().outof();
    readTicketHolder().setPeakUsed_forTest(size);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down since concurrency is already at its maximum allowed value, even though
    // ticktes are exhausted.
    _run();
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_LT(readTicketHolder().outof(), size);
    ASSERT_LT(writeTicketHolder().outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, NoProbeDown) {
    // Tickets are not exhausted.
    auto size = readTicketHolder().outof();
    readTicketHolder().setPeakUsed_forTest(size - 1);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Do not probe in either direction since tickets are not exhausted but concurrency is
    // already at its minimum allowed value.
    _run();
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_EQ(readTicketHolder().outof(), size);
    ASSERT_EQ(writeTicketHolder().outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, StepSizeNonZero) {
    gStepMultiple.store(0.1);
    // This value is chosen so that it takes two iterations to increase the stable concurrency by 1.
    gConcurrencyMovingAverageWeight.store(0.3);
    auto initialSize = readTicketHolder().outof();
    auto size = initialSize;

    // The concurrency level is low enough that the step multiple on its own is not enough to get to
    // the next integer.
    ASSERT_EQ(std::lround(size * (1 + gStepMultiple.load())), size);

    // Tickets are exhausted.
    readTicketHolder().setPeakUsed_forTest(size);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size + 1);
    ASSERT_EQ(writeTicketHolder().outof(), size + 1);

    // Throughput inreases.
    readTicketHolder().setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is not enough to increase concurrency yet.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size);
    ASSERT_EQ(writeTicketHolder().outof(), size);
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();

    // Run another iteration.

    // Tickets are exhausted.
    readTicketHolder().setPeakUsed_forTest(size);
    readTicketHolder().setNumFinishedProcessing_forTest(4);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size + 1);
    ASSERT_EQ(writeTicketHolder().outof(), size + 1);

    // Throughput inreases.
    readTicketHolder().setNumFinishedProcessing_forTest(6);
    _tick();

    // Probing up succeeds; the new value is finally enough to increase concurrency.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), size + 1);
    ASSERT_EQ(writeTicketHolder().outof(), size + 1);
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ReadWriteRatio) {
    gReadWriteRatio.store(0.67);  // 33% of tickets for writes, 67% for reads
    ON_BLOCK_EXIT([]() { gReadWriteRatio.store(0.5); });

    auto initialReads = readTicketHolder().outof();
    auto reads = initialReads;
    auto initialWrites = writeTicketHolder().outof();
    auto writes = initialWrites;

    // Initially these should be equal.
    ASSERT_EQ(reads, writes);

    // Write tickets are exhausted
    writeTicketHolder().setPeakUsed_forTest(writes);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. We expect write tickets to drop because
    // now the ratio is being applied. Total tickets should still increase.
    _run();
    ASSERT_GT(readTicketHolder().outof(), reads);
    ASSERT_LT(writeTicketHolder().outof(), writes);
    ASSERT_GT(readTicketHolder().outof() + writeTicketHolder().outof(), reads + writes);

    // There should be an imbalance.
    ASSERT_GT(readTicketHolder().outof(), writeTicketHolder().outof());

    reads = readTicketHolder().outof();
    writes = writeTicketHolder().outof();

    // Throughput inreases.
    readTicketHolder().setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(readTicketHolder().outof(), reads);
    ASSERT_GT(readTicketHolder().outof(), initialReads);
    ASSERT_LT(writeTicketHolder().outof(), writes);
    ASSERT_LT(writeTicketHolder().outof(), initialWrites);
    ASSERT_GT(readTicketHolder().outof() + writeTicketHolder().outof(),
              initialReads + initialWrites);
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();

    // This imbalance should still exist.
    ASSERT_GT(readTicketHolder().outof(), writeTicketHolder().outof());
}

TEST_F(ThroughputProbingReadHeavyTest, StepSizeNonZeroIncreasing) {
    auto reads = readTicketHolder().outof();
    auto writes = writeTicketHolder().outof();
    ASSERT_GT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for writes.
    ASSERT_EQ(std::lround(writes * (1 + gStepMultiple.load())), writes);

    // Write tickets are exhausted.
    writeTicketHolder().setPeakUsed_forTest(writes);
    writeTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. The number of write tickets should still
    // go up by 1.
    _run();
    ASSERT_GT(readTicketHolder().outof(), reads);
    ASSERT_EQ(writeTicketHolder().outof(), writes + 1);
}

TEST_F(ThroughputProbingReadHeavyTest, StepSizeNonZeroDecreasing) {
    auto reads = readTicketHolder().outof();
    auto writes = writeTicketHolder().outof();
    ASSERT_GT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for writes.
    ASSERT_EQ(std::lround(writes * (1 - gStepMultiple.load())), writes);

    // Tickets are not exhausted.
    readTicketHolder().setPeakUsed_forTest(reads - 1);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted. The number of write tickets should
    // still go down by 1.
    _run();
    ASSERT_LT(readTicketHolder().outof(), reads);
    ASSERT_EQ(writeTicketHolder().outof(), writes - 1);
}

TEST_F(ThroughputProbingWriteHeavyTest, StepSizeNonZeroIncreasing) {
    auto reads = readTicketHolder().outof();
    auto writes = writeTicketHolder().outof();
    ASSERT_LT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for reads.
    ASSERT_EQ(std::lround(reads * (1 + gStepMultiple.load())), reads);

    // Read tickets are exhausted.
    readTicketHolder().setPeakUsed_forTest(reads);
    readTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. The number of read tickets should still
    // go up by 1.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), reads + 1);
    ASSERT_GT(writeTicketHolder().outof(), writes);
}

TEST_F(ThroughputProbingWriteHeavyTest, StepSizeNonZeroDecreasing) {
    auto reads = readTicketHolder().outof();
    auto writes = writeTicketHolder().outof();
    ASSERT_LT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for reads.
    ASSERT_EQ(std::lround(reads * (1 + gStepMultiple.load())), reads);

    // Tickets are not exhausted.
    writeTicketHolder().setPeakUsed_forTest(writes - 1);
    writeTicketHolder().setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted. The number of read tickets should
    // still go down by 1.
    _run();
    ASSERT_EQ(readTicketHolder().outof(), reads - 1);
    ASSERT_LT(writeTicketHolder().outof(), writes);
}

TEST_F(ThroughputProbingTest, SetPeriodUpdatesProbingInterval) {
    // Verify initial period matches the configured interval.
    ASSERT_EQ(throughputProbing().getPeriod(),
              Milliseconds{gConcurrencyAdjustmentIntervalMillis.load()});

    // Update period to a new value.
    Milliseconds newPeriod{500};
    throughputProbing().setPeriod(newPeriod);

    // Verify period was updated.
    ASSERT_EQ(throughputProbing().getPeriod(), newPeriod);
}

}  // namespace mongo::admission::execution_control::throughput_probing
