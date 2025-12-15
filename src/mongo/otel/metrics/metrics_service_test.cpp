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

#include "mongo/otel/metrics/metrics_service.h"

#include "mongo/config.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/meter.h>

namespace mongo::otel::metrics {
namespace {


class MetricsServiceTest : public ServiceContextTest {};

// Assert that when a valid MeterProvider in place, we create a working Meter implementation with
// the expected metadata.
TEST_F(MetricsServiceTest, MeterIsInitialized) {
    // Set up a valid MeterProvider.
    OtelMetricsCapturer metricsCapturer;

    const auto& metricsService = MetricsService::get(getServiceContext());
    auto* meter = metricsService.getMeter_forTest();
    ASSERT_TRUE(meter);

    auto* sdkMeter = dynamic_cast<opentelemetry::sdk::metrics::Meter*>(meter);
    ASSERT_TRUE(sdkMeter);

    const auto* scope = sdkMeter->GetInstrumentationScope();
    ASSERT_TRUE(scope);
    ASSERT_EQ(scope->GetName(), std::string{MetricsService::kMeterName});
}

// Assert that we create a NoopMeter if the global MeterProvider hasn't been set.
TEST_F(MetricsServiceTest, ServiceContextInitBeforeMeterProvider) {
    const auto& metricsService = MetricsService::get(getServiceContext());
    auto* meter = metricsService.getMeter_forTest();
    ASSERT_TRUE(isNoopMeter(meter));
}

}  // namespace
}  // namespace mongo::otel::metrics
