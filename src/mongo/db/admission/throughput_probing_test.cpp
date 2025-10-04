/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/admission/throughput_probing.h"

#include "mongo/base/string_data.h"
#include "mongo/db/admission/throughput_probing_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
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

namespace mongo::admission::throughput_probing {
namespace {

TEST(ThroughputProbingParameterTest, InitialConcurrency) {
    ASSERT_OK(validateInitialConcurrency(gMinConcurrency, {}));
    ASSERT_OK(validateInitialConcurrency(gMaxConcurrency.load(), {}));
    ASSERT_NOT_OK(validateInitialConcurrency(gMinConcurrency - 1, {}));
    ASSERT_NOT_OK(validateInitialConcurrency(gMaxConcurrency.load() + 1, {}));
}

TEST(ThroughputProbingParameterTest, MinConcurrency) {
    ASSERT_OK(validateMinConcurrency(5, {}));
    ASSERT_OK(validateMinConcurrency(gMaxConcurrency.load(), {}));
    ASSERT_NOT_OK(validateMinConcurrency(0, {}));
    ASSERT_NOT_OK(validateMinConcurrency(gMaxConcurrency.load() + 1, {}));
}

TEST(ThroughputProbingParameterTest, MaxConcurrency) {
    ASSERT_OK(validateMaxConcurrency(gMinConcurrency, {}));
    ASSERT_OK(validateMaxConcurrency(256, {}));
    ASSERT_NOT_OK(validateMaxConcurrency(gMinConcurrency - 1, {}));
}

class ThroughputProbingTest : public ServiceContextTest {
protected:
    explicit ThroughputProbingTest(int32_t size = 64, double readWriteRatio = 0.5)
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Microseconds>>()))) {
        _svcCtx->setPeriodicRunner(std::make_unique<MockPeriodicRunner>());

        // The ThroughputProbing constructor requires the periodic runner to be set up on _svcCtx.
        throughput_probing::gInitialConcurrency = size;
        throughput_probing::gReadWriteRatio.store(readWriteRatio);
        _throughputProbing = std::make_unique<ThroughputProbing>(
            _svcCtx, &_readTicketHolder, &_writeTicketHolder, Milliseconds{1});

        // We need to advance the ticks to something other than zero, since that is used to
        // determine the if we're in the first iteration or not.
        _tick();

        // First loop is a no-op and initializes state.
        _run();
    }

    void _run() {
        _runner()->run(getClient());
        _statsTester.set(*_throughputProbing);
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

    ServiceContext* _svcCtx{getServiceContext()};
    TicketHolder _readTicketHolder{
        _svcCtx, 0, true /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth};
    TicketHolder _writeTicketHolder{
        _svcCtx, 0, true /* trackPeakUsed */, TicketHolder::kDefaultMaxQueueDepth};
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
    ThroughputProbingMaxConcurrencyTest() : ThroughputProbingTest(gMaxConcurrency.load() * 2) {}
};

class ThroughputProbingMinConcurrencyTest : public ThroughputProbingTest {
protected:
    // This input is the total initial concurrency between both ticketholders, so it will be split
    // evenly between each ticketholder. We are attempting to test a limit that is per-ticketholder.
    ThroughputProbingMinConcurrencyTest() : ThroughputProbingTest(gMinConcurrency * 2) {}
};

class ThroughputProbingReadHeavyTest : public ThroughputProbingTest {
protected:
    ThroughputProbingReadHeavyTest() : ThroughputProbingTest(16, 0.9) {}
};

class ThroughputProbingWriteHeavyTest : public ThroughputProbingTest {
protected:
    ThroughputProbingWriteHeavyTest() : ThroughputProbingTest(16, 0.1) {}
};

TEST_F(ThroughputProbingTest, ProbeUpSucceeds) {
    // Tickets are exhausted.
    auto initialSize = _readTicketHolder.outof();
    auto size = initialSize;
    _readTicketHolder.setPeakUsed_forTest(size);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), size);
    ASSERT_GT(_writeTicketHolder.outof(), size);

    // Throughput inreases.
    size = _readTicketHolder.outof();
    _readTicketHolder.setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_GT(_readTicketHolder.outof(), initialSize);
    ASSERT_LT(_writeTicketHolder.outof(), size);
    ASSERT_GT(_writeTicketHolder.outof(), initialSize);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeUpFails) {
    // Tickets are exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setPeakUsed_forTest(size);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), size);
    ASSERT_GT(_writeTicketHolder.outof(), size);

    // Throughput does not increase.
    _readTicketHolder.setNumFinishedProcessing_forTest(2);
    _tick();

    // Probing up fails since throughput did not increase. Return to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeDownSucceeds) {
    // Tickets are not exhausted.
    auto initialSize = _readTicketHolder.outof();
    auto size = initialSize;
    _readTicketHolder.setPeakUsed_forTest(size - 1);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);

    // Throughput increases.
    size = _readTicketHolder.outof();
    _readTicketHolder.setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), initialSize);
    ASSERT_GT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), initialSize);
    ASSERT_GT(_writeTicketHolder.outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyDecreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ProbeDownFails) {
    // Tickets are not exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setPeakUsed_forTest(size - 1);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);

    // Throughput does not increase.
    _readTicketHolder.setNumFinishedProcessing_forTest(2);
    _tick();

    // Probing down fails since throughput did not increase. Return back to stable.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();
}

TEST_F(ThroughputProbingMaxConcurrencyTest, NoProbeUp) {
    // Tickets are exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setPeakUsed_forTest(size);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down since concurrency is already at its maximum allowed value, even though
    // ticktes are exhausted.
    _run();
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedUp"));
    ASSERT_LT(_readTicketHolder.outof(), size);
    ASSERT_LT(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, NoProbeDown) {
    // Tickets are not exhausted.
    auto size = _readTicketHolder.outof();
    _readTicketHolder.setPeakUsed_forTest(size - 1);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Do not probe in either direction since tickets are not exhausted but concurrency is
    // already at its minimum allowed value.
    _run();
    ASSERT_TRUE(_statsTester.probeIncemented("timesProbedStable"));
    ASSERT_FALSE(_statsTester.probeIncemented("timesProbedDown"));
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
}

TEST_F(ThroughputProbingMinConcurrencyTest, StepSizeNonZero) {
    gStepMultiple.store(0.1);
    // This value is chosen so that it takes two iterations to increase the stable concurrency by 1.
    gConcurrencyMovingAverageWeight.store(0.3);
    auto initialSize = _readTicketHolder.outof();
    auto size = initialSize;

    // The concurrency level is low enough that the step multiple on its own is not enough to get to
    // the next integer.
    ASSERT_EQ(std::lround(size * (1 + gStepMultiple.load())), size);

    // Tickets are exhausted.
    _readTicketHolder.setPeakUsed_forTest(size);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size + 1);
    ASSERT_EQ(_writeTicketHolder.outof(), size + 1);

    // Throughput inreases.
    _readTicketHolder.setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is not enough to increase concurrency yet.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size);
    ASSERT_EQ(_writeTicketHolder.outof(), size);
    ASSERT(_statsTester.concurrencyKept()) << _statsTester.toString();

    // Run another iteration.

    // Tickets are exhausted.
    _readTicketHolder.setPeakUsed_forTest(size);
    _readTicketHolder.setNumFinishedProcessing_forTest(4);
    _tick();

    // Stable. Probe up next since tickets are exhausted.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size + 1);
    ASSERT_EQ(_writeTicketHolder.outof(), size + 1);

    // Throughput inreases.
    _readTicketHolder.setNumFinishedProcessing_forTest(6);
    _tick();

    // Probing up succeeds; the new value is finally enough to increase concurrency.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), size + 1);
    ASSERT_EQ(_writeTicketHolder.outof(), size + 1);
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();
}

TEST_F(ThroughputProbingTest, ReadWriteRatio) {
    gReadWriteRatio.store(0.67);  // 33% of tickets for writes, 67% for reads
    ON_BLOCK_EXIT([]() { gReadWriteRatio.store(0.5); });

    auto initialReads = _readTicketHolder.outof();
    auto reads = initialReads;
    auto initialWrites = _writeTicketHolder.outof();
    auto writes = initialWrites;

    // Initially these should be equal.
    ASSERT_EQ(reads, writes);

    // Write tickets are exhausted
    _writeTicketHolder.setPeakUsed_forTest(writes);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. We expect write tickets to drop because
    // now the ratio is being applied. Total tickets should still increase.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), reads);
    ASSERT_LT(_writeTicketHolder.outof(), writes);
    ASSERT_GT(_readTicketHolder.outof() + _writeTicketHolder.outof(), reads + writes);

    // There should be an imbalance.
    ASSERT_GT(_readTicketHolder.outof(), _writeTicketHolder.outof());

    reads = _readTicketHolder.outof();
    writes = _writeTicketHolder.outof();

    // Throughput inreases.
    _readTicketHolder.setNumFinishedProcessing_forTest(3);
    _tick();

    // Probing up succeeds; the new value is somewhere between the initial value and the probed-up
    // value.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), reads);
    ASSERT_GT(_readTicketHolder.outof(), initialReads);
    ASSERT_LT(_writeTicketHolder.outof(), writes);
    ASSERT_LT(_writeTicketHolder.outof(), initialWrites);
    ASSERT_GT(_readTicketHolder.outof() + _writeTicketHolder.outof(), initialReads + initialWrites);
    ASSERT(_statsTester.concurrencyIncreased()) << _statsTester.toString();

    // This imbalance should still exist.
    ASSERT_GT(_readTicketHolder.outof(), _writeTicketHolder.outof());
}

TEST_F(ThroughputProbingReadHeavyTest, StepSizeNonZeroIncreasing) {
    auto reads = _readTicketHolder.outof();
    auto writes = _writeTicketHolder.outof();
    ASSERT_GT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for writes.
    ASSERT_EQ(std::lround(writes * (1 + gStepMultiple.load())), writes);

    // Write tickets are exhausted.
    _writeTicketHolder.setPeakUsed_forTest(writes);
    _writeTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. The number of write tickets should still
    // go up by 1.
    _run();
    ASSERT_GT(_readTicketHolder.outof(), reads);
    ASSERT_EQ(_writeTicketHolder.outof(), writes + 1);
}

TEST_F(ThroughputProbingReadHeavyTest, StepSizeNonZeroDecreasing) {
    auto reads = _readTicketHolder.outof();
    auto writes = _writeTicketHolder.outof();
    ASSERT_GT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for writes.
    ASSERT_EQ(std::lround(writes * (1 - gStepMultiple.load())), writes);

    // Tickets are not exhausted.
    _readTicketHolder.setPeakUsed_forTest(reads - 1);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted. The number of write tickets should
    // still go down by 1.
    _run();
    ASSERT_LT(_readTicketHolder.outof(), reads);
    ASSERT_EQ(_writeTicketHolder.outof(), writes - 1);
}

TEST_F(ThroughputProbingWriteHeavyTest, StepSizeNonZeroIncreasing) {
    auto reads = _readTicketHolder.outof();
    auto writes = _writeTicketHolder.outof();
    ASSERT_LT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for reads.
    ASSERT_EQ(std::lround(reads * (1 + gStepMultiple.load())), reads);

    // Read tickets are exhausted.
    _readTicketHolder.setPeakUsed_forTest(reads);
    _readTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe up next since tickets are exhausted. The number of read tickets should still
    // go up by 1.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), reads + 1);
    ASSERT_GT(_writeTicketHolder.outof(), writes);
}

TEST_F(ThroughputProbingWriteHeavyTest, StepSizeNonZeroDecreasing) {
    auto reads = _readTicketHolder.outof();
    auto writes = _writeTicketHolder.outof();
    ASSERT_LT(reads, writes);

    // The concurrency level and read/write ratio are such that the step multiple on its own is not
    // enough to get to the next integer for reads.
    ASSERT_EQ(std::lround(reads * (1 + gStepMultiple.load())), reads);

    // Tickets are not exhausted.
    _writeTicketHolder.setPeakUsed_forTest(writes - 1);
    _writeTicketHolder.setNumFinishedProcessing_forTest(1);
    _tick();

    // Stable. Probe down next since tickets are not exhausted. The number of read tickets should
    // still go down by 1.
    _run();
    ASSERT_EQ(_readTicketHolder.outof(), reads - 1);
    ASSERT_LT(_writeTicketHolder.outof(), writes);
}

}  // namespace
}  // namespace mongo::admission::throughput_probing
