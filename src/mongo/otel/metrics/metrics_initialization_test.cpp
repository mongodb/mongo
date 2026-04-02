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

#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>

namespace mongo::otel::metrics {
namespace {

using testing::HasSubstr;
using unittest::match::StatusIs;

class OtelMetricsInitializationTest : public unittest::Test {
public:
    void setUp() override {
        opentelemetry::metrics::Provider::SetMeterProvider(
            std::make_shared<opentelemetry::metrics::NoopMeterProvider>());
    }

    void tearDown() override {
        shutdown();
    }

    const std::string& getMetricsDir() {
        return _tempMetricsDir.path();
    }

    std::string getPrometheusMetricsPath() {
        return _tempMetricsDir.path() + "/mongodb-prometheus-metrics.txt";
    }

private:
    unittest::TempDir _tempMetricsDir{"otel_metrics_test"};
};

TEST_F(OtelMetricsInitializationTest, NoMeterProvider) {
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, MultipleInitializationIsError) {
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());
    EXPECT_THAT(initialize(),
                StatusIs(ErrorCodes::IllegalOperation,
                         HasSubstr("initialization attempted after a previous initialization")));
}


TEST_F(OtelMetricsInitializationTest, Shutdown) {
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
    ASSERT_NOT_EQUALS(provider.get(), nullptr);

    shutdown();
    provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_EQ(provider.get(), nullptr);
}

TEST_F(OtelMetricsInitializationTest, FileMeterProvider) {
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpMeterProvider) {
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsHttpEndpoint",
                                               "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpAndDirectory) {
    RAIIServerParameterControllerForTest httpParam{"openTelemetryMetricsHttpEndpoint",
                                                   "http://localhost:4318/v1/traces"};
    RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                        getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusExporterPath) {
    RAIIServerParameterControllerForTest param{"openTelemetryPrometheusMetricsPath",
                                               getPrometheusMetricsPath()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusExporterDirectory) {
    RAIIServerParameterControllerForTest param{"openTelemetryPrometheusMetricsDirectory",
                                               getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusPathAndOtelDirectory) {
    RAIIServerParameterControllerForTest prometheusParam{"openTelemetryPrometheusMetricsPath",
                                                         getPrometheusMetricsPath()};
    RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                        getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryExporterMeterProvider) {
    RAIIServerParameterControllerForTest param{"openTelemetryPrometheusMetricsDirectory",
                                               getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryAndOtelDirectory) {
    RAIIServerParameterControllerForTest prometheusParam{"openTelemetryPrometheusMetricsDirectory",
                                                         getMetricsDir()};
    RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                        getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryDoesNotExistFails) {

    {
        RAIIServerParameterControllerForTest dirParam{"openTelemetryPrometheusMetricsDirectory",
                                                      "/nonexistent/directory"};
        ASSERT_THAT(initialize(),
                    StatusIs(ErrorCodes::FileOpenFailed, HasSubstr("/nonexistent/directory")));
    }
    {
        RAIIServerParameterControllerForTest pathParam{"openTelemetryPrometheusMetricsPath",
                                                       "/nonexistent/directory/file.prom"};
        ASSERT_THAT(initialize(),
                    StatusIs(ErrorCodes::FileOpenFailed, HasSubstr("/nonexistent/directory")));
    }
}

// Verify that openTelemetryPrometheusMetricsPath takes precedence over
// openTelemetryPrometheusMetricsDirectory when both are set. If the directory were used instead,
// the initialization would fail because the directory does not exist.
TEST_F(OtelMetricsInitializationTest, PrometheusPathTakesPrecedenceOverDirectory) {
    RAIIServerParameterControllerForTest pathParam{"openTelemetryPrometheusMetricsPath",
                                                   getPrometheusMetricsPath()};
    RAIIServerParameterControllerForTest dirParam{"openTelemetryPrometheusMetricsDirectory",
                                                  "/nonexistent/directory"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledNoParams) {
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagOtelMetrics", false};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledDirectorySet) {
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagOtelMetrics", false};
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledHttpSet) {
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagOtelMetrics", false};
    RAIIServerParameterControllerForTest param{"openTelemetryMetricsHttpEndpoint",
                                               "http://localhost:4318/v1/traces"};
    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, InvalidCompressionParam) {
    {
        RAIIServerParameterControllerForTest httpParam{"openTelemetryMetricsHttpEndpoint",
                                                       "http://localhost:4318/v1/traces"};
        RAIIServerParameterControllerForTest compressionParam{"openTelemetryMetricsCompression",
                                                              "foo"};
        ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
        ASSERT_TRUE(isNoopMeterProvider(provider.get()));
    }

    {
        RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                            getMetricsDir()};
        for (const auto& value : {"gzip", "foo"}) {
            RAIIServerParameterControllerForTest compressionParam{"openTelemetryMetricsCompression",
                                                                  value};
            ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_TRUE(isNoopMeterProvider(provider.get()));
        }
    }

    {
        RAIIServerParameterControllerForTest directoryParam{"openTelemetryPrometheusMetricsPath",
                                                            getPrometheusMetricsPath()};
        for (const auto& value : {"gzip", "foo"}) {
            RAIIServerParameterControllerForTest compressionParam{"openTelemetryMetricsCompression",
                                                                  value};
            ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_TRUE(isNoopMeterProvider(provider.get()));
        }
    }
}

TEST_F(OtelMetricsInitializationTest, ValidCompressionParam) {
    {
        RAIIServerParameterControllerForTest httpParam{"openTelemetryMetricsHttpEndpoint",
                                                       "http://localhost:4318/v1/traces"};
        for (const auto& value : {"gzip", "none"}) {
            RAIIServerParameterControllerForTest compressionParam{"openTelemetryMetricsCompression",
                                                                  value};
            ASSERT_OK(initialize());

            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_FALSE(isNoopMeterProvider(provider.get()));
            ASSERT_NOT_EQUALS(provider.get(), nullptr);

            shutdown();
        }
    }

    RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                        getMetricsDir()};
    RAIIServerParameterControllerForTest compressionParam{"openTelemetryMetricsCompression",
                                                          "none"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
    ASSERT_NOT_EQUALS(provider.get(), nullptr);

    shutdown();
}

TEST_F(OtelMetricsInitializationTest, TimeoutGreaterThanIntervalFails) {
    RAIIServerParameterControllerForTest directoryParam{"openTelemetryMetricsDirectory",
                                                        getMetricsDir()};
    // Set timeout greater than interval (interval defaults to 1000, timeout defaults to 500)
    RAIIServerParameterControllerForTest intervalParam{"openTelemetryExportIntervalMillis", 500};
    RAIIServerParameterControllerForTest timeoutParam{"openTelemetryExportTimeoutMillis", 1000};

    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}
}  // namespace
}  // namespace mongo::otel::metrics

#endif
