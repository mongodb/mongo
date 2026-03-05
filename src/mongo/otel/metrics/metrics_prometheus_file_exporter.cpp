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

#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <opentelemetry/exporters/prometheus/exporter_utils.h>
#include <opentelemetry/sdk/common/exporter_utils.h>
#include <opentelemetry/sdk/metrics/export/metric_producer.h>
#include <opentelemetry/sdk/metrics/instruments.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <prometheus/metric_family.h>
#include <prometheus/text_serializer.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo::otel::metrics {
namespace {
using ::opentelemetry::sdk::common::ExportResult;
using ::opentelemetry::sdk::metrics::AggregationTemporality;
using ::opentelemetry::sdk::metrics::InstrumentType;
using ::opentelemetry::sdk::metrics::PushMetricExporter;
using ::opentelemetry::sdk::metrics::ResourceMetrics;
using ::prometheus::MetricFamily;
using ::prometheus::TextSerializer;

/**
 * This failpoint calls the provided test callback via _testOnlyUnlockAndCallCallback. It is called
 * immediately after the writer thread wakes up from something.
 */
MONGO_FAIL_POINT_DEFINE(metricsPrometheusFileExporterThreadCallback);

Counter<int64_t>& successfulWritesCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kPrometheusFileExporterWrites,
    "The number of times the Prometheus file exporter succeeded writing out new metrics.",
    MetricUnit::kEvents,
    {.inServerStatus = true});

Counter<int64_t>& failedWritesCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kPrometheusFileExporterWritesFailed,
    "The number of times the Prometheus file exporter attempted to write out new metrics but "
    "failed for some reason.",
    MetricUnit::kEvents,
    {.inServerStatus = true});

Counter<int64_t>& skippedWritesCounter = MetricsService::instance().createInt64Counter(
    MetricNames::kPrometheusFileExporterWritesSkipped,
    "The number of times the Prometheus file exporter writer thread did not attempt to write out a "
    "set of metrics before the OTel framework provided a new set of metrics to write.",
    MetricUnit::kEvents,
    {.inServerStatus = true});

/**
 * Exports Opentelemetry metrics in Prometheus format to a file. This exports all metrics to the
 * file and overwrites the file every time there are new metrics to export.
 */
class PrometheusFileExporter : public PushMetricExporter {
public:
    explicit PrometheusFileExporter(const std::string& filepath,
                                    PrometheusFileExporterOptions options);
    ~PrometheusFileExporter() override;

    /**
     * Attempts to initialize the exporter (i.e., start the writer thread). Must be called prior to
     * calling `Export`, and may only be called once.
     */
    Status initialize();

    ExportResult Export(const ResourceMetrics& data) noexcept override;

    AggregationTemporality GetAggregationTemporality(
        InstrumentType instrument_type) const noexcept override {
        // We always return cumulative since we will be overwriting files and we don't want data to
        // be lost if the reader misses an update to the file.
        return AggregationTemporality::kCumulative;
    }

    bool ForceFlush(
        std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

    bool Shutdown(
        std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

private:
    /**
     * Starts the background thread that watches for new data to write and writes it.
     */
    Status _startFileWriterThread();

    /**
     * Writes metrics to the file.
     */
    Status _writeMetrics(const std::vector<MetricFamily>& metrics);

    /**
     * Checks if we've exceeded the limit for consecutive failures and dies if so.
     */
    void _checkConsecutiveFailures(WithLock lock);

    /**
     * Called from the failpoint, unlocks the lock, calls the provided callback, and locks again.
     */
    void _testOnlyUnlockAndCallCallback(stdx::unique_lock<stdx::timed_mutex>& lock);

    // The name of the file to export to. This will export first to a file with this name + .tmp,
    // then rename it to this name.
    const std::string _filepath;
    const std::string _tempFilepath;
    const int _maxConsecutiveFailures;
    TextSerializer _serializer;
    stdx::thread _writer;
    unique_function<void()> _testOnlyFailpointCallback;

    // Guards _condition, _metricsToWrite, the bools, and _consecutiveFailures.
    stdx::timed_mutex _mutex;
    stdx::condition_variable _condition;
    // The metrics that will be next exported.
    boost::optional<std::vector<MetricFamily>> _metricsToWrite;
    bool _shutdownStarted = false;
    bool _shutdownComplete = false;
    bool _flushNeeded = false;
    int _consecutiveFailures = 0;
};

PrometheusFileExporter::PrometheusFileExporter(const std::string& filepath,
                                               PrometheusFileExporterOptions options)
    : _filepath(filepath),
      _tempFilepath(fmt::format("{}.tmp", _filepath)),
      _maxConsecutiveFailures(options.maxConsecutiveFailures),
      _testOnlyFailpointCallback(std::move(options.testOnlyFailpointCallback)) {}

PrometheusFileExporter::~PrometheusFileExporter() {
    // If there was an error during initialization we may not have started the writer thread.
    if (_writer.joinable()) {
        Shutdown();
        _writer.join();
    }
}

Status PrometheusFileExporter::initialize() {
    // Make sure we didn't start the writer.
    invariant(!_writer.joinable());
    return _startFileWriterThread();
}

void PrometheusFileExporter::_checkConsecutiveFailures(WithLock lock) {
    if (_consecutiveFailures > _maxConsecutiveFailures) {
        LOGV2_FATAL(11990702,
                    "Exceeded the threshold for maximum allowed consecutive metric export failures",
                    "consecutiveFailures"_attr = _consecutiveFailures);
    }
}

Status PrometheusFileExporter::_startFileWriterThread() {
    // Try writing empty metrics to ensure we can write to the file.
    if (Status status = _writeMetrics(/*metrics=*/{}); !status.isOK()) {
        return status;
    }
    _writer = stdx::thread([&]() {
        setThreadName("PrometheusFileExporter");
        stdx::unique_lock lock(_mutex);
        while (true) {
            metricsPrometheusFileExporterThreadCallback.execute(
                [this, &lock](const BSONObj& data) { _testOnlyUnlockAndCallCallback(lock); });
            if (_shutdownStarted) {
                _shutdownComplete = true;
                _condition.notify_all();
                break;
            }

            if (_metricsToWrite.has_value()) {
                boost::optional<std::vector<MetricFamily>> toWrite;
                using std::swap;
                swap(toWrite, _metricsToWrite);
                // Unlock so that we don't block other metrics exports while we write to the file.
                lock.unlock();
                Status status = _writeMetrics(*toWrite);
                if (!status.isOK()) {
                    failedWritesCounter.add(1);
                    LOGV2_INFO(
                        11990700, "Error writing prometheus metrics file", "status"_attr = status);
                } else {
                    successfulWritesCounter.add(1);
                }
                // Destroy the metrics while we don't hold the lock.
                toWrite = boost::none;
                lock.lock();
                _consecutiveFailures = status.isOK() ? 0 : _consecutiveFailures + 1;
                _checkConsecutiveFailures(lock);
            }
            _flushNeeded = false;
            _condition.notify_all();

            MONGO_IDLE_THREAD_BLOCK;
            _condition.wait(lock, [this]() {
                return _shutdownStarted || _flushNeeded || _metricsToWrite.has_value();
            });
        }
    });
    return Status::OK();
}

Status PrometheusFileExporter::_writeMetrics(const std::vector<MetricFamily>& metrics) {
    // Writing the metrics is a 2 step process:
    // 1. Write the metrics to a temp file (_tempFilepath).
    // 2. Use filesystem::rename to rename the temp file to the actual metrics file (_filepath),
    //    thus updating the metrics.
    // filesystem::rename is atomic for our purposes: if a process opens the metrics file while the
    // rename is occurring, it will always see either the old file or the new file, but never a
    // combination of the two. Additionally, if a process is reading the old file when it is
    // overwritten by a new file, the old file will not be deleted from disk until the reader closes
    // the file handle.
    std::string serializedMetrics = _serializer.Serialize(metrics);
    std::ofstream filestream;
    filestream.open(_tempFilepath.c_str(), std::ios_base::out | std::ios_base::trunc);
    if (!filestream.is_open()) {
        return Status(ErrorCodes::FileOpenFailed,
                      fmt::format("Unable to open temp file to write metrics to. filepath: {}",
                                  _tempFilepath));
    }
    filestream.write(serializedMetrics.c_str(), serializedMetrics.length());
    filestream.close();
    if (filestream.fail()) {
        return Status(
            ErrorCodes::FileStreamFailed,
            fmt::format("Writing to metrics temp file failed. filepath: {}", _tempFilepath));
    }
    std::error_code ec;
    std::filesystem::rename(_tempFilepath, _filepath, ec);
    if (ec) {
        return Status(ErrorCodes::FileRenameFailed,
                      fmt::format("Error renaming temp metrics file to final metrics file. "
                                  "tempFilepath: {}, filepath: {}, error: {}",
                                  _tempFilepath,
                                  _filepath,
                                  ec.message()));
    }
    return Status::OK();
}

ExportResult PrometheusFileExporter::Export(const ResourceMetrics& data) noexcept {
    // Make sure we started the writer.
    invariant(_writer.joinable());
    std::vector<MetricFamily> convertedMetrics =
        opentelemetry::exporter::metrics::PrometheusExporterUtils::TranslateToPrometheus(data);
    stdx::lock_guard lock(_mutex);
    if (_metricsToWrite.has_value()) {
        skippedWritesCounter.add(1);
        LOGV2_INFO(11990701, "Skipping a set of metrics that were never written out");
        ++_consecutiveFailures;
        _checkConsecutiveFailures(lock);
    }
    _metricsToWrite = std::move(convertedMetrics);
    _condition.notify_all();
    return ExportResult::kSuccess;
}

bool PrometheusFileExporter::ForceFlush(std::chrono::microseconds timeout) noexcept {
    // TODO SERVER-120797: Handle timeouts
    // Make sure we started the writer.
    invariant(_writer.joinable());
    stdx::unique_lock lock(_mutex);
    _flushNeeded = true;
    _condition.notify_all();
    _condition.wait(lock, [this]() { return _flushNeeded == false; });
    return true;
}

bool PrometheusFileExporter::Shutdown(std::chrono::microseconds timeout) noexcept {
    // TODO SERVER-120797: Handle timeouts
    // If we never started the writer thread, Shutdown is trivial.
    if (!_writer.joinable()) {
        return true;
    }
    stdx::unique_lock lock(_mutex);
    _shutdownStarted = true;
    _condition.notify_all();
    _condition.wait(lock, [this]() { return _shutdownComplete; });
    return true;
}

void PrometheusFileExporter::_testOnlyUnlockAndCallCallback(
    stdx::unique_lock<stdx::timed_mutex>& lock) {
    lock.unlock();
    _testOnlyFailpointCallback();
    lock.lock();
}
}  // namespace

StatusWith<std::unique_ptr<PushMetricExporter>> createPrometheusFileExporter(
    const std::string& filepath, PrometheusFileExporterOptions options) {
    if (filepath.empty()) {
        return Status(ErrorCodes::BadValue, "filepath must be provided");
    }
    auto exporter = std::make_unique<PrometheusFileExporter>(filepath, std::move(options));
    if (Status status = exporter->initialize(); !status.isOK()) {
        return status;
    }
    return exporter;
}

}  // namespace mongo::otel::metrics
