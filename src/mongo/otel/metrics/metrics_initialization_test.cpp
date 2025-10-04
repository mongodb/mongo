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

#include "mongo/config.h"

#ifdef MONGO_CONFIG_OTEL

#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>

namespace mongo {
namespace otel {
namespace {

class OtelMetricsInitializationTest : public unittest::Test {
public:
    void setUp() override {
        opentelemetry::metrics::Provider::SetMeterProvider(
            std::make_shared<opentelemetry::metrics::NoopMeterProvider>());
        metrics::gOpenTelemetryMetricsHttpEndpoint.clear();
        metrics::gOpenTelemetryMetricsDirectory.clear();
    }

    void tearDown() override {
        opentelemetry::metrics::Provider::SetMeterProvider({});
    }
};

bool isNoop(opentelemetry::metrics::MeterProvider* provider) {
    return !!dynamic_cast<opentelemetry::metrics::NoopMeterProvider*>(provider);
}

TEST_F(OtelMetricsInitializationTest, NoMeterProvider) {
    ASSERT_OK(metrics::initialize("mongod"));

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoop(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, Shutdown) {
    metrics::gOpenTelemetryMetricsDirectory = "/tmp/";
    ASSERT_OK(metrics::initialize("mongod"));
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoop(provider.get()));
    ASSERT_NOT_EQUALS(provider.get(), nullptr);

    metrics::shutdown();
    provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_EQ(provider.get(), nullptr);
}

TEST_F(OtelMetricsInitializationTest, FileMeterProvider) {
    metrics::gOpenTelemetryMetricsDirectory = "/tmp/";
    ASSERT_OK(metrics::initialize("mongod"));

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpMeterProvider) {
    metrics::gOpenTelemetryMetricsHttpEndpoint = "http://localhost:4318/v1/traces";
    ASSERT_OK(metrics::initialize("mongod"));

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoop(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpAndDirectory) {
    metrics::gOpenTelemetryMetricsHttpEndpoint = "http://localhost:4318/v1/traces";
    metrics::gOpenTelemetryMetricsDirectory = "/tmp/";
    auto status = metrics::initialize("mongod");
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoop(provider.get()));
}

}  // namespace
}  // namespace otel
}  // namespace mongo

#endif
