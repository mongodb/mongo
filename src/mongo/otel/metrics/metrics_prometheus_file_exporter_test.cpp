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

#include "mongo/otel/metrics/metrics_prometheus_file_exporter.h"

#include "mongo/config.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <opentelemetry/sdk/instrumentationscope/instrumentation_scope.h>
#include <opentelemetry/sdk/metrics/data/metric_data.h>
#include <opentelemetry/sdk/metrics/export/metric_producer.h>
#include <opentelemetry/sdk/metrics/instruments.h>
#include <opentelemetry/sdk/resource/resource.h>

namespace mongo::otel::metrics {
namespace {

namespace otel_metrics = opentelemetry::sdk::metrics;
using otel_metrics::PushMetricExporter;
using otel_metrics::ResourceMetrics;

using ::opentelemetry::sdk::common::ExportResult;
using ::opentelemetry::sdk::instrumentationscope::InstrumentationScope;

using testing::_;
using testing::AllOf;
using testing::ContainsRegex;
using testing::HasSubstr;
using testing::Not;
using unittest::match::StatusIs;

std::string readFileContents(const std::string& path) {
    std::ifstream ifs(path);
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

/**
 * Creates a ResourceMetrics with a single sum point with the provided value.
 */
ResourceMetrics makeResourceMetrics(const std::string& metricName, int64_t value) {
    static auto& resource = opentelemetry::sdk::resource::Resource::GetEmpty();
    static std::unique_ptr<InstrumentationScope> scope = InstrumentationScope::Create("test_scope");

    otel_metrics::SumPointData sumPoint;
    sumPoint.value_ = value;
    sumPoint.is_monotonic_ = true;

    otel_metrics::PointDataAttributes pointAttr;
    pointAttr.point_data = sumPoint;

    otel_metrics::MetricData metricData;
    metricData.instrument_descriptor =
        otel_metrics::InstrumentDescriptor{.name_ = metricName,
                                           .description_ = "A test metric",
                                           .unit_ = "1",
                                           .type_ = otel_metrics::InstrumentType::kCounter,
                                           .value_type_ = otel_metrics::InstrumentValueType::kLong};
    metricData.aggregation_temporality = otel_metrics::AggregationTemporality::kCumulative;
    metricData.point_data_attr_.push_back(pointAttr);

    std::vector<otel_metrics::MetricData> metricDataVec;
    metricDataVec.push_back(std::move(metricData));

    otel_metrics::ScopeMetrics scopeMetrics(scope.get(), std::move(metricDataVec));

    std::vector<otel_metrics::ScopeMetrics> scopeMetricsVec;
    scopeMetricsVec.push_back(std::move(scopeMetrics));

    return ResourceMetrics(&resource, std::move(scopeMetricsVec));
}

class PrometheusFileExporterTest : public unittest::Test {
protected:
    /**
     * Returns a valid path that can be used for writing metrics.
     */
    std::string filepath() {
        return _tempDir.path() + "/metrics.prom";
    }

    /**
     * Asserts that the exporter status is ok and returns the exporter created at filepath().
     */
    std::unique_ptr<otel_metrics::PushMetricExporter> makeExporter(
        PrometheusFileExporterOptions options = {}) {
        StatusWith<std::unique_ptr<PushMetricExporter>> exporter =
            createPrometheusFileExporter(filepath(), std::move(options));
        ASSERT_OK(exporter.getStatus());
        return std::move(exporter.getValue());
    }

    unittest::TempDir _tempDir{"prom_file_exporter_test"};
};

TEST_F(PrometheusFileExporterTest, CreateSucceeds) {
    EXPECT_EQ(createPrometheusFileExporter(filepath()).getStatus(), Status::OK());
}

TEST_F(PrometheusFileExporterTest, CreateFailsWithNoPath) {
    EXPECT_THAT(createPrometheusFileExporter(/*filepath=*/"").getStatus(),
                StatusIs(ErrorCodes::BadValue, HasSubstr("filepath")));
}

TEST_F(PrometheusFileExporterTest, CreateFailsWithInvalidPath) {
    EXPECT_THAT(
        createPrometheusFileExporter(/*filepath=*/"/nonexistent/deeply/nested/dir/metrics.prom")
            .getStatus(),
        StatusIs(ErrorCodes::FileOpenFailed, _));
}

TEST_F(PrometheusFileExporterTest, ExportWritesMetricsToFile) {
    OtelMetricsCapturer metricsCapturer;
    std::unique_ptr<PushMetricExporter> exporter = makeExporter();

    EXPECT_EQ(exporter->Export(makeResourceMetrics("requests_total", 42)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    std::string contents = readFileContents(filepath());
    EXPECT_THAT(contents, ContainsRegex("requests_total.*42"));
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWrites), 1);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesFailed),
              0);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesSkipped),
              0);
}

TEST_F(PrometheusFileExporterTest, ExportEmptyMetrics) {
    std::unique_ptr<PushMetricExporter> exporter = makeExporter();
    ResourceMetrics emptyMetrics;
    EXPECT_EQ(exporter->Export(emptyMetrics), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_TRUE(std::filesystem::exists(filepath()));
}

TEST_F(PrometheusFileExporterTest, SubsequentExportOverwritesFile) {
    std::unique_ptr<PushMetricExporter> exporter = makeExporter();
    ASSERT_EQ(exporter->Export(makeResourceMetrics("first_metric", 10)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_THAT(readFileContents(filepath()), HasSubstr("first_metric"));

    ASSERT_EQ(exporter->Export(makeResourceMetrics("second_metric", 20)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_THAT(readFileContents(filepath()),
                AllOf(HasSubstr("second_metric"), Not(HasSubstr("first_metric"))));
}

TEST_F(PrometheusFileExporterTest, SkippedExportIncrementsCounter) {
    OtelMetricsCapturer metricsCapturer;
    // The writer thread blocks on this future until the test has exported twice, so that we can
    // guarantee that one export is skipped.
    auto [promise, future] = makePromiseFuture<void>();
    std::unique_ptr<PushMetricExporter> exporter =
        makeExporter(/*options=*/{.testOnlyFailpointCallback = [&future]() {
            future.wait();
        }});

    FailPointEnableBlock fp("metricsPrometheusFileExporterThreadCallback");
    ASSERT_EQ(exporter->Export(makeResourceMetrics("first_metric", 10)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("second_metric", 10)), ExportResult::kSuccess);

    ASSERT_THAT(readFileContents(filepath()), Not(HasSubstr("first_metric")));
    promise.emplaceValue();
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_THAT(readFileContents(filepath()),
                AllOf(HasSubstr("second_metric"), Not(HasSubstr("first_metric"))));
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWrites), 1);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesFailed),
              0);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesSkipped),
              1);
}

TEST_F(PrometheusFileExporterTest, TempFileIsCleanedUp) {
    std::string metricsPath = filepath();
    std::string tempPath = metricsPath + ".tmp";

    std::unique_ptr<PushMetricExporter> exporter = makeExporter();
    ASSERT_EQ(exporter->Export(makeResourceMetrics("cleanup_test", 1)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_TRUE(std::filesystem::exists(metricsPath));
    EXPECT_FALSE(std::filesystem::exists(tempPath));
}

TEST_F(PrometheusFileExporterTest, ForceFlushWithNoExport) {
    std::unique_ptr<PushMetricExporter> exporter = makeExporter();

    EXPECT_TRUE(exporter->ForceFlush());
    // Multiple flushes are fine.
    EXPECT_TRUE(exporter->ForceFlush());
}

TEST_F(PrometheusFileExporterTest, ShutdownSucceeds) {
    std::unique_ptr<PushMetricExporter> exporter = makeExporter();

    EXPECT_TRUE(exporter->Shutdown());
    // A second call should be fine.
    EXPECT_TRUE(exporter->Shutdown());
}

TEST_F(PrometheusFileExporterTest, InitFailsWhenTmpFileCannotBeCreated) {
    std::string metricsPath = filepath();
    std::string tmpPath = metricsPath + ".tmp";

    // Place a directory at the tmp file path so ofstream::open fails.
    std::filesystem::create_directory(tmpPath);

    EXPECT_THAT(createPrometheusFileExporter(metricsPath).getStatus(),
                StatusIs(ErrorCodes::FileOpenFailed, _));
}

TEST_F(PrometheusFileExporterTest, InitFailsWhenMetricsFileCannotBeCreated) {
    std::string metricsPath = filepath();

    // Place a directory at the metrics file path so rename(tmp, metrics) fails.
    std::filesystem::create_directory(metricsPath);

    EXPECT_THAT(createPrometheusFileExporter(metricsPath).getStatus(),
                StatusIs(ErrorCodes::FileRenameFailed, _));
}

TEST_F(PrometheusFileExporterTest, IntermediateTmpFileCreationFailurePreservesOldMetrics) {
    OtelMetricsCapturer metricsCapturer;
    std::string metricsPath = filepath();
    std::string tmpPath = metricsPath + ".tmp";

    std::unique_ptr<PushMetricExporter> exporter = makeExporter();

    ASSERT_EQ(exporter->Export(makeResourceMetrics("initial_metric", 1)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());
    ASSERT_THAT(readFileContents(metricsPath), HasSubstr("initial_metric"));

    // Block the tmp path with a directory so the next write fails to open it.
    std::filesystem::create_directory(tmpPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("new_metric", 2)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    // The old metrics file should be unchanged since the tmp write failed.
    EXPECT_THAT(readFileContents(metricsPath),
                AllOf(HasSubstr("initial_metric"), Not(HasSubstr("new_metric"))));

    // Remove the blocking directory and verify the exporter recovers.
    std::filesystem::remove(tmpPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("recovered_metric", 3)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_THAT(readFileContents(metricsPath), HasSubstr("recovered_metric"));
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWrites), 2);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesFailed),
              1);
}

TEST_F(PrometheusFileExporterTest, IntermediateRenameFailureDoesNotPreventFutureMetrics) {
    OtelMetricsCapturer metricsCapturer;
    std::string metricsPath = filepath();

    std::unique_ptr<PushMetricExporter> exporter = makeExporter();

    ASSERT_EQ(exporter->Export(makeResourceMetrics("initial_metric", 1)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());
    ASSERT_THAT(readFileContents(metricsPath), HasSubstr("initial_metric"));

    // Replace the metrics file with a directory so rename(tmp, metrics) fails.
    std::filesystem::remove(metricsPath);
    std::filesystem::create_directory(metricsPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("new_metric", 2)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    // The rename could not replace the directory, so it should still be there.
    EXPECT_TRUE(std::filesystem::is_directory(metricsPath));

    // Remove the blocking directory and verify the exporter recovers.
    std::filesystem::remove(metricsPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("recovered_metric", 3)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    EXPECT_THAT(readFileContents(metricsPath), HasSubstr("recovered_metric"));
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWrites), 2);
    EXPECT_EQ(metricsCapturer.readInt64Counter(MetricNames::kPrometheusFileExporterWritesFailed),
              1);
}

TEST_F(PrometheusFileExporterTest, ExactlyMaxConsecutiveFailuresIsOk) {
    std::string metricsPath = filepath();
    std::unique_ptr<PushMetricExporter> exporter = makeExporter({.maxConsecutiveFailures = 3});

    // Replace the metrics file with a directory so rename(tmp, metrics) fails.
    std::filesystem::remove(metricsPath);
    std::filesystem::create_directory(metricsPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 1)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 2)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 3)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());
    std::filesystem::remove(metricsPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 4)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());
}

using PrometheusFileExporterDeathTest = PrometheusFileExporterTest;

DEATH_TEST_F(PrometheusFileExporterDeathTest,
             ConsecutiveFailedExportsCauseDeath,
             "maximum allowed consecutive metric export failures") {
    std::string metricsPath = filepath();
    std::unique_ptr<PushMetricExporter> exporter = makeExporter({.maxConsecutiveFailures = 3});

    // Replace the metrics file with a directory so rename(tmp, metrics) fails.
    std::filesystem::remove(metricsPath);
    std::filesystem::create_directory(metricsPath);

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 1)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 2)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 3)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());

    ASSERT_EQ(exporter->Export(makeResourceMetrics("metric", 4)), ExportResult::kSuccess);
    ASSERT_TRUE(exporter->ForceFlush());
}

TEST_F(PrometheusFileExporterTest, ExactlyMaxConsecutiveSkipsIsOk) {
    // The writer thread blocks on this future.
    auto [promise, future] = makePromiseFuture<void>();
    std::unique_ptr<PushMetricExporter> exporter = makeExporter(
        /*options=*/{.maxConsecutiveFailures = 3, .testOnlyFailpointCallback = [&future]() {
                         future.wait();
                     }});

    FailPointEnableBlock fp("metricsPrometheusFileExporterThreadCallback");
    // The first export can never be skipped as the exporter has not exported previously.
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 1)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 2)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 3)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 4)), ExportResult::kSuccess);
    promise.emplaceValue();
    ASSERT_TRUE(exporter->ForceFlush());
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 5)), ExportResult::kSuccess);
}

DEATH_TEST_F(PrometheusFileExporterDeathTest,
             ConsecutiveSkippedExportsCauseDeath,
             "maximum allowed consecutive metric export failures") {
    // The writer thread blocks on this future.
    auto [promise, future] = makePromiseFuture<void>();
    std::unique_ptr<PushMetricExporter> exporter = makeExporter(
        /*options=*/{.maxConsecutiveFailures = 3, .testOnlyFailpointCallback = [&future]() {
                         future.wait();
                     }});

    FailPointEnableBlock fp("metricsPrometheusFileExporterThreadCallback");
    // The first export can never be skipped as the exporter has not exported previously.
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 1)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 2)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 3)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 4)), ExportResult::kSuccess);
    EXPECT_EQ(exporter->Export(makeResourceMetrics("metric", 5)), ExportResult::kSuccess);
}

}  // namespace
}  // namespace mongo::otel::metrics
