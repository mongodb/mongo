// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"

#ifdef MONGO_CONFIG_OTEL

#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/otel/metrics/metrics_settings_gen.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <chrono>
#include <vector>

#include <boost/filesystem.hpp>
#include <opentelemetry/exporters/otlp/otlp_file_client.h>
#include <opentelemetry/exporters/otlp/otlp_file_client_options.h>
#include <opentelemetry/exporters/otlp/otlp_file_client_runtime_options.h>
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/proto/resource/v1/resource.pb.h>

namespace mongo::otel::metrics {
namespace {

using testing::HasSubstr;
using testing::MatchesRegex;
using testing::UnorderedElementsAre;
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
    ASSERT_TRUE(isNoopMeterProvider(provider.get()));
}

TEST_F(OtelMetricsInitializationTest, FileMeterProvider) {
    unittest::ServerParameterGuard param{"openTelemetryMetricsDirectory", getMetricsDir()};
    ASSERT_OK(initialize());

    auto provider = opentelemetry::metrics::Provider::GetMeterProvider();
    ASSERT_FALSE(isNoopMeterProvider(provider.get()));
}

// The mongo shell's cat() refuses to load any file >= 16MB. The OTLP file metric exporter appends
// records to its output file, so without size-based rotation a long-running server (e.g. under
// TSAN) produces a single metrics file that exceeds this limit, breaking shell-based readers like
// jstests otel_metrics.js. These tests pin the exporter configuration that prevents that: a
// rotation index in the file name and a per-file size cap below the 16MB limit.
TEST_F(OtelMetricsInitializationTest, FileExporterConfigRotatesAndStaysUnderCatLimit) {
    // Must match kFileSizeLimit in src/mongo/shell/shell_utils_extended.cpp.
    constexpr std::size_t kCatFileSizeLimit = 16 * 1024 * 1024;

    auto config = metrics_initialization_detail::makeMetricsFileExporterConfig(
        "/var/log/mongodb/metrics", "12345");

    // The file name must carry the rotation index (%N) so the exporter rotates into distinct files
    // instead of appending to one unbounded file.
    ASSERT_STRING_CONTAINS(config.filePattern, "%N");
    ASSERT_TRUE(config.filePattern.ends_with("-metrics.jsonl"))
        << "unexpected pattern: " << config.filePattern;
    ASSERT_STRING_CONTAINS(config.filePattern, "/var/log/mongodb/metrics/mongodb-12345-");

    // Each rotated file must stay strictly below the shell cat() limit so tooling can read it.
    ASSERT_LT(config.fileSize, kCatFileSizeLimit)
        << "per-file size cap " << config.fileSize << " must be below the cat() limit "
        << kCatFileSizeLimit;
    ASSERT_GT(config.fileSize, 0);
}

// Verifies that the production file pattern's rotation index (%N) is expanded by the OTLP file
// client and that size-based rotation produces distinct, sequentially-numbered files. Uses the real
// exporter config but shrinks file_size so rotation happens after every record, then checks the
// resulting file names (mongodb-999-<date>-0-metrics.jsonl, -1-, -2-, ...).
TEST_F(OtelMetricsInitializationTest, DemoRotationFillsNPlaceholder) {
    namespace otlp = opentelemetry::exporter::otlp;
    const std::string dir = getMetricsDir();

    auto cfg = metrics_initialization_detail::makeMetricsFileExporterConfig(dir, "999");
    ASSERT_EQ(cfg.filePattern, fmt::format("{}/mongodb-999-%Y%m%d-%N-metrics.jsonl", dir));

    otlp::OtlpFileClientFileSystemOptions sysOpts;
    sysOpts.file_pattern = cfg.filePattern;  // real "...-%N-metrics.jsonl" pattern
    sysOpts.file_size = 1;                   // tiny -> rotate after each written record
    sysOpts.rotate_size = 3;
    sysOpts.flush_count = 1;

    otlp::OtlpFileClientOptions options;
    options.backend_options = sysOpts;
    otlp::OtlpFileClient client(std::move(options), otlp::OtlpFileClientRuntimeOptions{});

    opentelemetry::proto::resource::v1::Resource msg;
    for (int i = 0; i < 3; ++i) {
        client.Export(msg, 1);
    }
    client.ForceFlush(std::chrono::microseconds{5'000'000});

    std::vector<std::string> names;
    for (const auto& entry : boost::filesystem::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    EXPECT_THAT(names,
                UnorderedElementsAre(MatchesRegex(R"(mongodb-999-[0-9]{8}-0-metrics\.jsonl)"),
                                     MatchesRegex(R"(mongodb-999-[0-9]{8}-1-metrics\.jsonl)"),
                                     MatchesRegex(R"(mongodb-999-[0-9]{8}-2-metrics\.jsonl)")));
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
}  // namespace
}  // namespace mongo::otel::metrics

#endif
