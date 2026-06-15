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
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>

namespace mongo::otel::metrics {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Pair;
using testing::SizeIs;
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

class OtelMetricsHttpExportHeadersTest : public unittest::Test {
public:
    OpenTelemetryMetricsHttpExportHeaders headers{"openTelemetryMetricsHttpExportHeaders",
                                                  ServerParameterType::kStartupOnly};

    void setUp() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    void tearDown() override {
        ASSERT_OK(setHeaders(BSONObj{}));
    }

    Status setHeaders(BSONObj doc) {
        auto storage = BSON("v" << doc);
        return headers.set(storage.firstElement(), boost::none);
    }
};

TEST_F(OtelMetricsInitializationTest, NoMeterProvider) {
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, MultipleInitializationIsError) {
    unittest::ServerParameterGuard param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());
    EXPECT_THAT(initialize(),
                StatusIs(ErrorCodes::IllegalOperation,
                         HasSubstr("initialization attempted after a previous initialization")));
}


TEST_F(OtelMetricsInitializationTest, Shutdown) {
    unittest::ServerParameterGuard param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
    ASSERT_NOT_EQUALS(provider.get(), nullptr);

    shutdown();
    provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_EQ(provider.get(), nullptr);
}

TEST_F(OtelMetricsInitializationTest, FileMeterProvider) {
    unittest::ServerParameterGuard param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpMeterProvider) {
    unittest::ServerParameterGuard param{"openTelemetryMetricsHttpEndpoint",
                                         "http://localhost:4318/v1/traces"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, HttpAndDirectory) {
    unittest::ServerParameterGuard httpParam{"openTelemetryMetricsHttpEndpoint",
                                             "http://localhost:4318/v1/traces"};
    unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory", getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusExporterPath) {
    unittest::ServerParameterGuard param{"openTelemetryPrometheusMetricsPath",
                                         getPrometheusMetricsPath()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusExporterDirectory) {
    unittest::ServerParameterGuard param{"openTelemetryPrometheusMetricsDirectory",
                                         getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusPathAndOtelDirectory) {
    unittest::ServerParameterGuard prometheusParam{"openTelemetryPrometheusMetricsPath",
                                                   getPrometheusMetricsPath()};
    unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory", getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryExporterMeterProvider) {
    unittest::ServerParameterGuard param{"openTelemetryPrometheusMetricsDirectory",
                                         getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryAndOtelDirectory) {
    unittest::ServerParameterGuard prometheusParam{"openTelemetryPrometheusMetricsDirectory",
                                                   getMetricsDir()};
    unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory", getMetricsDir()};
    auto status = initialize();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.codeString(), "InvalidOptions");

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, PrometheusDirectoryDoesNotExistFails) {

    {
        unittest::ServerParameterGuard dirParam{"openTelemetryPrometheusMetricsDirectory",
                                                "/nonexistent/directory"};
        ASSERT_THAT(initialize(),
                    StatusIs(ErrorCodes::FileOpenFailed, HasSubstr("/nonexistent/directory")));
    }
    {
        unittest::ServerParameterGuard pathParam{"openTelemetryPrometheusMetricsPath",
                                                 "/nonexistent/directory/file.prom"};
        ASSERT_THAT(initialize(),
                    StatusIs(ErrorCodes::FileOpenFailed, HasSubstr("/nonexistent/directory")));
    }
}

// Verify that openTelemetryPrometheusMetricsPath takes precedence over
// openTelemetryPrometheusMetricsDirectory when both are set. If the directory were used instead,
// the initialization would fail because the directory does not exist.
TEST_F(OtelMetricsInitializationTest, PrometheusPathTakesPrecedenceOverDirectory) {
    unittest::ServerParameterGuard pathParam{"openTelemetryPrometheusMetricsPath",
                                             getPrometheusMetricsPath()};
    unittest::ServerParameterGuard dirParam{"openTelemetryPrometheusMetricsDirectory",
                                            "/nonexistent/directory"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledNoParams) {
    unittest::ServerParameterGuard featureFlagController{"featureFlagOtelMetrics", false};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledDirectorySet) {
    unittest::ServerParameterGuard featureFlagController{"featureFlagOtelMetrics", false};
    unittest::ServerParameterGuard param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FeatureFlagDisabledHttpSet) {
    unittest::ServerParameterGuard featureFlagController{"featureFlagOtelMetrics", false};
    unittest::ServerParameterGuard param{"openTelemetryMetricsHttpEndpoint",
                                         "http://localhost:4318/v1/traces"};
    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, InvalidCompressionParam) {
    {
        unittest::ServerParameterGuard httpParam{"openTelemetryMetricsHttpEndpoint",
                                                 "http://localhost:4318/v1/traces"};
        unittest::ServerParameterGuard compressionParam{"openTelemetryMetricsCompression", "foo"};
        ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
        ASSERT_TRUE(isNoopMeterProvider(provider.get()));
    }

    {
        unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory",
                                                      getMetricsDir()};
        for (const auto& value : {"gzip", "foo"}) {
            unittest::ServerParameterGuard compressionParam{"openTelemetryMetricsCompression",
                                                            value};
            ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_TRUE(isNoopMeterProvider(provider.get()));
        }
    }

    {
        unittest::ServerParameterGuard directoryParam{"openTelemetryPrometheusMetricsPath",
                                                      getPrometheusMetricsPath()};
        for (const auto& value : {"gzip", "foo"}) {
            unittest::ServerParameterGuard compressionParam{"openTelemetryMetricsCompression",
                                                            value};
            ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);
            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_TRUE(isNoopMeterProvider(provider.get()));
        }
    }
}

TEST_F(OtelMetricsInitializationTest, ValidCompressionParam) {
    {
        unittest::ServerParameterGuard httpParam{"openTelemetryMetricsHttpEndpoint",
                                                 "http://localhost:4318/v1/traces"};
        for (const auto& value : {"gzip", "none"}) {
            unittest::ServerParameterGuard compressionParam{"openTelemetryMetricsCompression",
                                                            value};
            ASSERT_OK(initialize());

            auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
            ASSERT_FALSE(isNoopMeterProvider(provider.get()));
            ASSERT_NOT_EQUALS(provider.get(), nullptr);

            shutdown();
        }
    }

    unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory", getMetricsDir()};
    unittest::ServerParameterGuard compressionParam{"openTelemetryMetricsCompression", "none"};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
    ASSERT_NOT_EQUALS(provider.get(), nullptr);

    shutdown();
}

TEST_F(OtelMetricsInitializationTest, TimeoutGreaterThanIntervalFails) {
    unittest::ServerParameterGuard directoryParam{"openTelemetryMetricsDirectory", getMetricsDir()};
    // Set timeout greater than interval (interval defaults to 1000, timeout defaults to 500)
    unittest::ServerParameterGuard intervalParam{"openTelemetryExportIntervalMillis", 500};
    unittest::ServerParameterGuard timeoutParam{"openTelemetryExportTimeoutMillis", 1000};

    ASSERT_EQ(initialize().code(), ErrorCodes::InvalidOptions);

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersSetFromBson) {
    // Verify headers can be set and are stored
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(1));

    // Verify headers can be updated
    ASSERT_OK(setHeaders(
        BSON("Authorization" << "Bearer tok" << "X-Tenant-ID" << "acme" << "X-Empty-Value" << "")));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(3));

    // Verify that an empty object causes headers to be cleared
    ASSERT_OK(setHeaders(BSONObj{}));
    EXPECT_THAT(getMetricsHttpExportHeaders(), IsEmpty());
}

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersSetFromString) {
    // Verify setFromString behavior
    ASSERT_OK(headers.setFromString("{\"Authorization\": \"Bearer tok\"}", boost::none));
    EXPECT_THAT(getMetricsHttpExportHeaders(), SizeIs(1));
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    EXPECT_THAT(headers.setFromString("{\"BadBson\":", boost::none),
                StatusIs(ErrorCodes::BadValue, HasSubstr("convert string to BSON")));
}

TEST_F(OtelMetricsHttpExportHeadersTest, HttpExportHeadersFailurePreservesPreviousValue) {
    ASSERT_OK(setHeaders(BSON("Authorization" << "Bearer tok")));
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));

    // BSON allows empty field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("" << "value")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("empty key")));
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(setHeaders(BSON("X-Same" << "first" << "X-Same" << "second")),
                StatusIs(ErrorCodes::BadValue, HasSubstr("duplicate key")));

    // Backing store must be unchanged after a failed set().
    EXPECT_THAT(getMetricsHttpExportHeaders(),
                ElementsAre(Pair("Authorization", ElementsAre("Bearer tok"))));
}
}  // namespace
}  // namespace mongo::otel::metrics

#endif
