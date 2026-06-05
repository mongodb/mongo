/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/top.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

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
                                             std::tuple{"read"_sd});
    ASSERT_EQ(data.sum, 500);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, WriteRecordsWithWriteOpType) {
    increment(Microseconds(1000), Command::ReadWriteType::kWrite);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"write"_sd});
    ASSERT_EQ(data.sum, 1000);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, CommandRecordsWithCommandOpType) {
    increment(Microseconds(250), Command::ReadWriteType::kCommand);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"command"_sd});
    ASSERT_EQ(data.sum, 250);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, TransactionDoesNotRecord) {
    // kTransaction is tracked separately via incrementForTransaction; not in this histogram.
    increment(Microseconds(999), Command::ReadWriteType::kTransaction);

    for (StringData opType : {"read"_sd, "write"_sd, "command"_sd}) {
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
                                                    std::tuple{"read"_sd}),
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
                                                    std::tuple{"read"_sd}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(ServiceLatencyTrackerOtelTest, RecordsLatencyNotWorkingTime) {
    // Call _tracker directly (not the increment() helper) to pass distinct latency and workingTime.
    _tracker.increment(
        _opCtx.get(), Microseconds(1000), Microseconds(200), Command::ReadWriteType::kRead);

    auto data = _capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                             std::tuple{"read"_sd});
    ASSERT_EQ(data.sum, 1000);
    ASSERT_EQ(data.count, 1UL);
}

TEST_F(ServiceLatencyTrackerOtelTest, DirectClientSkipsRecord) {
    _opCtx->getClient()->setInDirectClient(true);
    increment(Microseconds(500), Command::ReadWriteType::kRead);

    ASSERT_THROWS_CODE(_capturer.readInt64Histogram(otel::metrics::MetricNames::kOperationLatency,
                                                    std::tuple{"read"_sd}),
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
                                                    std::tuple{"read"_sd}),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

}  // namespace
}  // namespace mongo
