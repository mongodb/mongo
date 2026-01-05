/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/otel/metrics/metrics_test_util.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

class OtelMetricsCapturerTest : public ServiceContextTest {};

TEST_F(OtelMetricsCapturerTest, ReadThrowsExceptionIfMetricNotFound) {
    OtelMetricsCapturer metricsCapturer;
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Counter(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Histogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
    ASSERT_THROWS_CODE(metricsCapturer.readDoubleHistogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::KeyNotFound);
}

TEST_F(OtelMetricsCapturerTest, CorrectNameWrongTypeThrowsException) {
    OtelMetricsCapturer metricsCapturer;
    auto& metricsService = MetricsService::get(getServiceContext());
    Histogram<int64_t>* int64Histogram = metricsService.createInt64Histogram(
        MetricNames::kTest1, "description1", MetricUnit::kSeconds);
    Histogram<double>* doubleHistogram = metricsService.createDoubleHistogram(
        MetricNames::kTest2, "description1", MetricUnit::kSeconds);
    // A value must be recorded for the histogram to be initialized in the underlying metrics
    // exporter.
    int64Histogram->record(1);
    doubleHistogram->record(1);

    ASSERT_THROWS_CODE(metricsCapturer.readDoubleHistogram(MetricNames::kTest1),
                       DBException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(metricsCapturer.readInt64Histogram(MetricNames::kTest2),
                       DBException,
                       ErrorCodes::TypeMismatch);
}
}  // namespace mongo::otel::metrics
