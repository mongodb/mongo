// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
