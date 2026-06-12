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

#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/storage_engine_mock.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

class WriteConcernGleMetricsTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto* service = getServiceContext();
        service->setStorageEngine(std::make_unique<StorageEngineMock>());
        _opCtx = cc().makeOperationContext();

        repl::ReplSettings settings;
        settings.setReplSetString("mySet/node1:12345");
        auto mockReplCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);
        _mockReplCoord = mockReplCoord.get();
        repl::ReplicationCoordinator::set(service, std::move(mockReplCoord));
    }

protected:
    // Builds a WriteConcernOptions with w:2 and j:false (NONE sync mode) so
    // waitForWriteConcern exercises the replication wait path without needing a
    // journal flusher.
    WriteConcernOptions makeW2WriteConcern(ReadWriteConcernProvenance::Source source) {
        WriteConcernOptions wc;
        wc.w = WriteConcernW{int64_t{2}};
        wc.syncMode = WriteConcernOptions::SyncMode::NONE;
        wc.getProvenance().setSource(source);
        return wc;
    }

    // Configures the mock replication coordinator so that awaitReplication() returns
    // the given status and duration.
    void setMockAwaitReplicationResult(Status status, Milliseconds duration) {
        _mockReplCoord->setAwaitReplicationReturnValueFunction(
            [status = std::move(status), duration](OperationContext*, const repl::OpTime&) {
                return repl::ReplicationCoordinator::StatusAndDuration(status, duration);
            });
    }

    repl::ReplicationCoordinatorMock* _mockReplCoord;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(WriteConcernGleMetricsTest, SuccessfulWaitIncrementsWtimeMetrics) {
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    otel::metrics::OtelMetricsCapturer capturer;

    setMockAwaitReplicationResult(Status::OK(), Milliseconds(50));

    auto wc = makeW2WriteConcern(ReadWriteConcernProvenance::Source::clientSupplied);
    WriteConcernResult result;
    ASSERT_OK(waitForWriteConcern(_opCtx.get(), {{1, 1}, 1}, wc, &result));

    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorWtimeNum), 1);
    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorWtimeTotalMillis),
              50);
}

TEST_F(WriteConcernGleMetricsTest, ClientSuppliedTimeoutOnlyIncrementsWtimeouts) {
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    otel::metrics::OtelMetricsCapturer capturer;

    setMockAwaitReplicationResult(Status(ErrorCodes::WriteConcernTimeout, "mock timeout"),
                                  Milliseconds(10));

    auto wc = makeW2WriteConcern(ReadWriteConcernProvenance::Source::clientSupplied);
    WriteConcernResult result;
    auto status = waitForWriteConcern(_opCtx.get(), {{1, 1}, 1}, wc, &result);
    ASSERT_EQ(status, ErrorCodes::WriteConcernTimeout);
    ASSERT_TRUE(result.wTimedOut);

    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorWtimeouts), 1);
    // The default.wtimeouts metric must not be incremented for a client-supplied
    // write concern.
    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorDefaultWtimeouts),
              0);
}

TEST_F(WriteConcernGleMetricsTest, NonClientSuppliedTimeoutIncrementsBothWtimeoutsMetrics) {
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    otel::metrics::OtelMetricsCapturer capturer;

    setMockAwaitReplicationResult(Status(ErrorCodes::WriteConcernTimeout, "mock timeout"),
                                  Milliseconds(10));

    auto wc = makeW2WriteConcern(ReadWriteConcernProvenance::Source::implicitDefault);
    WriteConcernResult result;
    auto status = waitForWriteConcern(_opCtx.get(), {{1, 1}, 1}, wc, &result);
    ASSERT_EQ(status, ErrorCodes::WriteConcernTimeout);

    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorWtimeouts), 1);
    ASSERT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorDefaultWtimeouts),
              1);
}

TEST_F(WriteConcernGleMetricsTest, NonClientSuppliedUnsatisfiableIncrementsDefaultUnsatisfiable) {
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    otel::metrics::OtelMetricsCapturer capturer;

    setMockAwaitReplicationResult(
        Status(ErrorCodes::UnsatisfiableWriteConcern, "mock unsatisfiable"), Milliseconds(0));

    auto wc = makeW2WriteConcern(ReadWriteConcernProvenance::Source::implicitDefault);
    WriteConcernResult result;
    auto status = waitForWriteConcern(_opCtx.get(), {{1, 1}, 1}, wc, &result);
    ASSERT_EQ(status, ErrorCodes::UnsatisfiableWriteConcern);

    ASSERT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorDefaultUnsatisfiable),
        1);
}

TEST_F(WriteConcernGleMetricsTest,
       ClientSuppliedUnsatisfiableDoesNotIncrementDefaultUnsatisfiable) {
    if (!otel::metrics::OtelMetricsCapturer::canReadMetrics()) {
        return;
    }
    otel::metrics::OtelMetricsCapturer capturer;

    setMockAwaitReplicationResult(
        Status(ErrorCodes::UnsatisfiableWriteConcern, "mock unsatisfiable"), Milliseconds(0));

    auto wc = makeW2WriteConcern(ReadWriteConcernProvenance::Source::clientSupplied);
    WriteConcernResult result;
    auto status = waitForWriteConcern(_opCtx.get(), {{1, 1}, 1}, wc, &result);
    ASSERT_EQ(status, ErrorCodes::UnsatisfiableWriteConcern);

    ASSERT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kGetLastErrorDefaultUnsatisfiable),
        0);
}

}  // namespace
}  // namespace mongo
