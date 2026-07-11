// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/top.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <string_view>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

/**
 * Fixture with a transport session so isFromUserConnection() returns true.
 */
class ServiceLatencyTrackerOtelTest : public ServiceContextTest {
public:
    ServiceLatencyTrackerOtelTest()
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(/*shouldSetupTL=*/false),
              transport::MockSession::create(/*transportLayer=*/nullptr)),
          _opCtx{makeOperationContext()} {}

    void SetUp() override {
        if (!_capturer.canReadMetrics())
            GTEST_SKIP() << "OTel metrics reader unavailable";
    }

    void increment(Microseconds latency, Command::ReadWriteType rwType) {
        _tracker.increment(_opCtx.get(), latency, latency, rwType);
    }

protected:
    otel::metrics::OtelMetricsCapturer _capturer;
    ServiceLatencyTracker _tracker;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ServiceLatencyTrackerOtelTest, ReadRecordsWithReadOpType) {
    increment(Microseconds(500), Command::ReadWriteType::kRead);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"read"sv});
    ASSERT_EQ(data.sum, 500);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, WriteRecordsWithWriteOpType) {
    increment(Microseconds(1000), Command::ReadWriteType::kWrite);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"write"sv});
    ASSERT_EQ(data.sum, 1000);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, CommandRecordsWithCommandOpType) {
    increment(Microseconds(250), Command::ReadWriteType::kCommand);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"command"sv});
    ASSERT_EQ(data.sum, 250);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, TransactionDoesNotRecord) {
    // kTransaction is tracked separately via incrementForTransaction; not in this histogram.
    increment(Microseconds(999), Command::ReadWriteType::kTransaction);

    for (auto opType : {"read"sv, "write"sv, "command"sv}) {
        ASSERT_THROWS_CODE(_capturer.readInt64Histogram(
                               otel::metrics::MetricNames::kOperationLatency, std::tuple{opType}),
                           DBException,
                           ErrorCodes::KeyNotFound);
    }
}

TEST_F(ServiceLatencyTrackerOtelTest, ShouldIncrementLatencyStatsFalseSkipsRecord) {
    _opCtx->setShouldIncrementLatencyStats(false);
    increment(Microseconds(500), Command::ReadWriteType::kRead);

    ASSERT_THROWS_CODE(_capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                                    std::tuple{"read"sv}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(ServiceLatencyTrackerOtelTest, QESuppressionSkipsRecord) {
    {
        std::lock_guard<Client> clientLock(*_opCtx->getClient());
        CurOp::get(_opCtx.get())->setShouldOmitDiagnosticInformation(clientLock, true);
    }
    increment(Microseconds(500), Command::ReadWriteType::kRead);

    ASSERT_THROWS_CODE(_capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                                    std::tuple{"read"sv}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(ServiceLatencyTrackerOtelTest, RecordsLatencyNotWorkingTime) {
    // Call _tracker directly (not the increment() helper) to pass distinct latency and workingTime.
    _tracker.increment(
        _opCtx.get(), Microseconds(1000), Microseconds(200), Command::ReadWriteType::kRead);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"read"sv});
    ASSERT_EQ(data.sum, 1000);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, DirectClientSkipsRecord) {
    _opCtx->getClient()->setInDirectClient(true);
    increment(Microseconds(500), Command::ReadWriteType::kRead);

    ASSERT_THROWS_CODE(_capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                                    std::tuple{"read"sv}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

/**
 * Fixture without a transport session — isFromUserConnection() returns false.
 */
class ServiceLatencyTrackerOtelNonUserTest : public ServiceContextTest {
public:
    ServiceLatencyTrackerOtelNonUserTest() : ServiceContextTest(), _opCtx{makeOperationContext()} {}

    void SetUp() override {
        if (!_capturer.canReadMetrics())
            GTEST_SKIP() << "OTel metrics reader unavailable";
    }

protected:
    otel::metrics::OtelMetricsCapturer _capturer;
    ServiceLatencyTracker _tracker;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ServiceLatencyTrackerOtelNonUserTest, NonUserConnectionSkipsRecord) {
    ASSERT_FALSE(_opCtx->getClient()->isFromUserConnection());
    _tracker.increment(
        _opCtx.get(), Microseconds(500), Microseconds(500), Command::ReadWriteType::kRead);

    ASSERT_THROWS_CODE(_capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                                    std::tuple{"read"sv}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

}  // namespace
}  // namespace mongo
