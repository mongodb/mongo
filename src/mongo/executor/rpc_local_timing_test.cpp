// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/rpc_local_timing.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

#include <array>
#include <string_view>
#include <tuple>

namespace mongo::executor {
namespace {

class RpcLocalTimingMetricTest : public unittest::Test {
protected:
    void setUp() override {
        if (!_capturer.canReadMetrics())
            GTEST_SKIP() << "OTel metrics reader unavailable";
    }

    /** Returns the sample count recorded for the given kind, or 0 if none yet. */
    int64_t countFor(RpcLocalTimingKind kind, RpcLocalTimingMode mode) {
        try {
            return static_cast<int64_t>(
                _capturer
                    .readInt64Histogram(otel::metrics::MetricNames::kNetworkRpcLocalLatency,
                                        std::tuple{toString(kind), toString(mode)})
                    .count);
        } catch (const DBException&) {
            // The histogram does not exist until the first sample is recorded.
            return 0;
        }
    }

    /** Returns the sum (micros) recorded for the given kind, or 0 if none yet. */
    int64_t sumFor(RpcLocalTimingKind kind, RpcLocalTimingMode mode) {
        try {
            return _capturer
                .readInt64Histogram(otel::metrics::MetricNames::kNetworkRpcLocalLatency,
                                    std::tuple{toString(kind), toString(mode)})
                .sum;
        } catch (const DBException&) {
            // The histogram does not exist until the first sample is recorded.
            return 0;
        }
    }

    TickSourceMock<Microseconds> clock;
    RpcLocalTiming timing{&clock};

private:
    otel::metrics::OtelMetricsCapturer _capturer;
};

TEST_F(RpcLocalTimingMetricTest, RecordsInitialSegment) {
    clock.advance(Microseconds{5000});
    recordRpcLocalLatency(timing, RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync);

    EXPECT_EQ(countFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 1);
    EXPECT_EQ(sumFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 5000);
}

TEST_F(RpcLocalTimingMetricTest, RecordsExhaustContinuationSegment) {
    clock.advance(Microseconds{2500});
    recordRpcLocalLatency(
        timing, RpcLocalTimingKind::kExhaustContinuation, RpcLocalTimingMode::kAsync);

    EXPECT_EQ(countFor(RpcLocalTimingKind::kExhaustContinuation, RpcLocalTimingMode::kAsync), 1);
    EXPECT_EQ(sumFor(RpcLocalTimingKind::kExhaustContinuation, RpcLocalTimingMode::kAsync), 2500);
}

TEST_F(RpcLocalTimingMetricTest, RoutesAllKindAndModePairsToTheirOwnSeries) {
    constexpr std::array kinds{RpcLocalTimingKind::kInitial,
                               RpcLocalTimingKind::kExhaustContinuation,
                               RpcLocalTimingKind::kFireAndForget};
    constexpr std::array modes{RpcLocalTimingMode::kAsync, RpcLocalTimingMode::kSync};

    size_t index = 0;
    for (const auto kind : kinds) {
        for (const auto mode : modes) {
            const auto duration = Microseconds{static_cast<int64_t>(index + 1) * 100};
            RpcLocalTiming sample(&clock);
            clock.advance(duration);
            recordRpcLocalLatency(sample, kind, mode);
            ++index;
        }
    }

    index = 0;
    for (const auto kind : kinds) {
        for (const auto mode : modes) {
            SCOPED_TRACE(fmt::format("kind={}, mode={}", toString(kind), toString(mode)));
            EXPECT_EQ(countFor(kind, mode), 1);
            EXPECT_EQ(sumFor(kind, mode), static_cast<int64_t>(index + 1) * 100);
            ++index;
        }
    }
}

TEST_F(RpcLocalTimingMetricTest, RepeatedCallRecordsNothing) {
    clock.advance(Microseconds{7500});
    recordRpcLocalLatency(timing, RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync);
    recordRpcLocalLatency(timing, RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kSync);
    clock.advance(Microseconds{1000});
    recordRpcLocalLatency(
        timing, RpcLocalTimingKind::kExhaustContinuation, RpcLocalTimingMode::kAsync);

    EXPECT_EQ(countFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 1);
    EXPECT_EQ(countFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kSync), 0);
    EXPECT_EQ(sumFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 7500);
    EXPECT_EQ(countFor(RpcLocalTimingKind::kExhaustContinuation, RpcLocalTimingMode::kAsync), 0);
}

TEST_F(RpcLocalTimingMetricTest, ZeroLengthSegmentRecords) {
    recordRpcLocalLatency(timing, RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync);

    EXPECT_EQ(countFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 1);
    EXPECT_EQ(sumFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 0);
}

TEST_F(RpcLocalTimingMetricTest, NegativeLengthSegmentClampedToZero) {
    recordRpcLocalLatency(timing, RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync);

    EXPECT_EQ(countFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 1);
    clock.advance(Milliseconds{-5});
    EXPECT_EQ(sumFor(RpcLocalTimingKind::kInitial, RpcLocalTimingMode::kAsync), 0);
}

}  // namespace
}  // namespace mongo::executor
