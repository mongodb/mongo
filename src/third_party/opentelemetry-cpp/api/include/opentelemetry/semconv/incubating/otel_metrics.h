/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_metrics-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace otel
{

/**
  The number of log records for which the export has finished, either successful or failed.
  <p>
  For successful exports, @code error.type @endcode MUST NOT be set. For failed exports, @code
  error.type @endcode MUST contain the failure cause. For exporters with partial success semantics
  (e.g. OTLP with @code rejected_log_records @endcode), rejected log records MUST count as failed
  and only non-rejected log records count as success. If no rejection reason is available, @code
  rejected @endcode SHOULD be used as value for @code error.type @endcode. <p> counter
 */
static constexpr const char *kMetricOtelSdkExporterLogExported = "otel.sdk.exporter.log.exported";
static constexpr const char *descrMetricOtelSdkExporterLogExported =
    "The number of log records for which the export has finished, either successful or failed.";
static constexpr const char *unitMetricOtelSdkExporterLogExported = "{log_record}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkExporterLogExported(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkExporterLogExported,
                                    descrMetricOtelSdkExporterLogExported,
                                    unitMetricOtelSdkExporterLogExported);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkExporterLogExported(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkExporterLogExported,
                                    descrMetricOtelSdkExporterLogExported,
                                    unitMetricOtelSdkExporterLogExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterLogExported(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkExporterLogExported,
                                             descrMetricOtelSdkExporterLogExported,
                                             unitMetricOtelSdkExporterLogExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterLogExported(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkExporterLogExported,
                                              descrMetricOtelSdkExporterLogExported,
                                              unitMetricOtelSdkExporterLogExported);
}

/**
  The number of log records which were passed to the exporter, but that have not been exported yet
  (neither successful, nor failed). <p> For successful exports, @code error.type @endcode MUST NOT
  be set. For failed exports, @code error.type @endcode MUST contain the failure cause. <p>
  updowncounter
 */
static constexpr const char *kMetricOtelSdkExporterLogInflight = "otel.sdk.exporter.log.inflight";
static constexpr const char *descrMetricOtelSdkExporterLogInflight =
    "The number of log records which were passed to the exporter, but that have not been exported "
    "yet (neither successful, nor failed).";
static constexpr const char *unitMetricOtelSdkExporterLogInflight = "{log_record}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterLogInflight(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterLogInflight,
                                         descrMetricOtelSdkExporterLogInflight,
                                         unitMetricOtelSdkExporterLogInflight);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterLogInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterLogInflight,
                                          descrMetricOtelSdkExporterLogInflight,
                                          unitMetricOtelSdkExporterLogInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterLogInflight(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkExporterLogInflight,
                                                   descrMetricOtelSdkExporterLogInflight,
                                                   unitMetricOtelSdkExporterLogInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterLogInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkExporterLogInflight,
                                                    descrMetricOtelSdkExporterLogInflight,
                                                    unitMetricOtelSdkExporterLogInflight);
}

/**
  The number of metric data points for which the export has finished, either successful or failed.
  <p>
  For successful exports, @code error.type @endcode MUST NOT be set. For failed exports, @code
  error.type @endcode MUST contain the failure cause. For exporters with partial success semantics
  (e.g. OTLP with @code rejected_data_points @endcode), rejected data points MUST count as failed
  and only non-rejected data points count as success. If no rejection reason is available, @code
  rejected @endcode SHOULD be used as value for @code error.type @endcode. <p> counter
 */
static constexpr const char *kMetricOtelSdkExporterMetricDataPointExported =
    "otel.sdk.exporter.metric_data_point.exported";
static constexpr const char *descrMetricOtelSdkExporterMetricDataPointExported =
    "The number of metric data points for which the export has finished, either successful or "
    "failed.";
static constexpr const char *unitMetricOtelSdkExporterMetricDataPointExported = "{data_point}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkExporterMetricDataPointExported(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkExporterMetricDataPointExported,
                                    descrMetricOtelSdkExporterMetricDataPointExported,
                                    unitMetricOtelSdkExporterMetricDataPointExported);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkExporterMetricDataPointExported(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkExporterMetricDataPointExported,
                                    descrMetricOtelSdkExporterMetricDataPointExported,
                                    unitMetricOtelSdkExporterMetricDataPointExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterMetricDataPointExported(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkExporterMetricDataPointExported,
                                             descrMetricOtelSdkExporterMetricDataPointExported,
                                             unitMetricOtelSdkExporterMetricDataPointExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterMetricDataPointExported(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkExporterMetricDataPointExported,
                                              descrMetricOtelSdkExporterMetricDataPointExported,
                                              unitMetricOtelSdkExporterMetricDataPointExported);
}

/**
  The number of metric data points which were passed to the exporter, but that have not been
  exported yet (neither successful, nor failed). <p> For successful exports, @code error.type
  @endcode MUST NOT be set. For failed exports, @code error.type @endcode MUST contain the failure
  cause. <p> updowncounter
 */
static constexpr const char *kMetricOtelSdkExporterMetricDataPointInflight =
    "otel.sdk.exporter.metric_data_point.inflight";
static constexpr const char *descrMetricOtelSdkExporterMetricDataPointInflight =
    "The number of metric data points which were passed to the exporter, but that have not been "
    "exported yet (neither successful, nor failed).";
static constexpr const char *unitMetricOtelSdkExporterMetricDataPointInflight = "{data_point}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterMetricDataPointInflight(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterMetricDataPointInflight,
                                         descrMetricOtelSdkExporterMetricDataPointInflight,
                                         unitMetricOtelSdkExporterMetricDataPointInflight);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterMetricDataPointInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterMetricDataPointInflight,
                                          descrMetricOtelSdkExporterMetricDataPointInflight,
                                          unitMetricOtelSdkExporterMetricDataPointInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterMetricDataPointInflight(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOtelSdkExporterMetricDataPointInflight,
      descrMetricOtelSdkExporterMetricDataPointInflight,
      unitMetricOtelSdkExporterMetricDataPointInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterMetricDataPointInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOtelSdkExporterMetricDataPointInflight,
      descrMetricOtelSdkExporterMetricDataPointInflight,
      unitMetricOtelSdkExporterMetricDataPointInflight);
}

/**
  The duration of exporting a batch of telemetry records.
  <p>
  This metric defines successful operations using the full success definitions for <a
  href="https://github.com/open-telemetry/opentelemetry-proto/blob/v1.5.0/docs/specification.md#full-success-1">http</a>
  and <a
  href="https://github.com/open-telemetry/opentelemetry-proto/blob/v1.5.0/docs/specification.md#full-success">grpc</a>.
  Anything else is defined as an unsuccessful operation. For successful operations, @code error.type
  @endcode MUST NOT be set. For unsuccessful export operations, @code error.type @endcode MUST
  contain a relevant failure cause. <p> histogram
 */
static constexpr const char *kMetricOtelSdkExporterOperationDuration =
    "otel.sdk.exporter.operation.duration";
static constexpr const char *descrMetricOtelSdkExporterOperationDuration =
    "The duration of exporting a batch of telemetry records.";
static constexpr const char *unitMetricOtelSdkExporterOperationDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricOtelSdkExporterOperationDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricOtelSdkExporterOperationDuration,
                                      descrMetricOtelSdkExporterOperationDuration,
                                      unitMetricOtelSdkExporterOperationDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricOtelSdkExporterOperationDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricOtelSdkExporterOperationDuration,
                                      descrMetricOtelSdkExporterOperationDuration,
                                      unitMetricOtelSdkExporterOperationDuration);
}

/**
  The number of spans for which the export has finished, either successful or failed.
  <p>
  For successful exports, @code error.type @endcode MUST NOT be set. For failed exports, @code
  error.type @endcode MUST contain the failure cause. For exporters with partial success semantics
  (e.g. OTLP with @code rejected_spans @endcode), rejected spans MUST count as failed and only
  non-rejected spans count as success. If no rejection reason is available, @code rejected @endcode
  SHOULD be used as value for @code error.type @endcode. <p> counter
 */
static constexpr const char *kMetricOtelSdkExporterSpanExported = "otel.sdk.exporter.span.exported";
static constexpr const char *descrMetricOtelSdkExporterSpanExported =
    "The number of spans for which the export has finished, either successful or failed.";
static constexpr const char *unitMetricOtelSdkExporterSpanExported = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanExported(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkExporterSpanExported,
                                    descrMetricOtelSdkExporterSpanExported,
                                    unitMetricOtelSdkExporterSpanExported);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanExported(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkExporterSpanExported,
                                    descrMetricOtelSdkExporterSpanExported,
                                    unitMetricOtelSdkExporterSpanExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanExported(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkExporterSpanExported,
                                             descrMetricOtelSdkExporterSpanExported,
                                             unitMetricOtelSdkExporterSpanExported);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanExported(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkExporterSpanExported,
                                              descrMetricOtelSdkExporterSpanExported,
                                              unitMetricOtelSdkExporterSpanExported);
}

/**
  Deprecated, use @code otel.sdk.exporter.span.exported @endcode instead.

  @deprecated
  {"note": "Replaced by @code otel.sdk.exporter.span.exported @endcode.", "reason": "renamed",
  "renamed_to": "otel.sdk.exporter.span.exported"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkExporterSpanExportedCount =
    "otel.sdk.exporter.span.exported.count";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricOtelSdkExporterSpanExportedCount =
    "Deprecated, use `otel.sdk.exporter.span.exported` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkExporterSpanExportedCount =
    "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterSpanExportedCount,
                                         descrMetricOtelSdkExporterSpanExportedCount,
                                         unitMetricOtelSdkExporterSpanExportedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterSpanExportedCount,
                                          descrMetricOtelSdkExporterSpanExportedCount,
                                          unitMetricOtelSdkExporterSpanExportedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkExporterSpanExportedCount,
                                                   descrMetricOtelSdkExporterSpanExportedCount,
                                                   unitMetricOtelSdkExporterSpanExportedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkExporterSpanExportedCount,
                                                    descrMetricOtelSdkExporterSpanExportedCount,
                                                    unitMetricOtelSdkExporterSpanExportedCount);
}

/**
  The number of spans which were passed to the exporter, but that have not been exported yet
  (neither successful, nor failed). <p> For successful exports, @code error.type @endcode MUST NOT
  be set. For failed exports, @code error.type @endcode MUST contain the failure cause. <p>
  updowncounter
 */
static constexpr const char *kMetricOtelSdkExporterSpanInflight = "otel.sdk.exporter.span.inflight";
static constexpr const char *descrMetricOtelSdkExporterSpanInflight =
    "The number of spans which were passed to the exporter, but that have not been exported yet "
    "(neither successful, nor failed).";
static constexpr const char *unitMetricOtelSdkExporterSpanInflight = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanInflight(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterSpanInflight,
                                         descrMetricOtelSdkExporterSpanInflight,
                                         unitMetricOtelSdkExporterSpanInflight);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterSpanInflight,
                                          descrMetricOtelSdkExporterSpanInflight,
                                          unitMetricOtelSdkExporterSpanInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanInflight(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkExporterSpanInflight,
                                                   descrMetricOtelSdkExporterSpanInflight,
                                                   unitMetricOtelSdkExporterSpanInflight);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanInflight(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkExporterSpanInflight,
                                                    descrMetricOtelSdkExporterSpanInflight,
                                                    unitMetricOtelSdkExporterSpanInflight);
}

/**
  Deprecated, use @code otel.sdk.exporter.span.inflight @endcode instead.

  @deprecated
  {"note": "Replaced by @code otel.sdk.exporter.span.inflight @endcode.", "reason": "renamed",
  "renamed_to": "otel.sdk.exporter.span.inflight"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkExporterSpanInflightCount =
    "otel.sdk.exporter.span.inflight.count";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricOtelSdkExporterSpanInflightCount =
    "Deprecated, use `otel.sdk.exporter.span.inflight` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkExporterSpanInflightCount =
    "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                         descrMetricOtelSdkExporterSpanInflightCount,
                                         unitMetricOtelSdkExporterSpanInflightCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                          descrMetricOtelSdkExporterSpanInflightCount,
                                          unitMetricOtelSdkExporterSpanInflightCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                                   descrMetricOtelSdkExporterSpanInflightCount,
                                                   unitMetricOtelSdkExporterSpanInflightCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                                    descrMetricOtelSdkExporterSpanInflightCount,
                                                    unitMetricOtelSdkExporterSpanInflightCount);
}

/**
  The number of logs submitted to enabled SDK Loggers.
  <p>
  counter
 */
static constexpr const char *kMetricOtelSdkLogCreated = "otel.sdk.log.created";
static constexpr const char *descrMetricOtelSdkLogCreated =
    "The number of logs submitted to enabled SDK Loggers.";
static constexpr const char *unitMetricOtelSdkLogCreated = "{log_record}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricOtelSdkLogCreated(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkLogCreated, descrMetricOtelSdkLogCreated,
                                    unitMetricOtelSdkLogCreated);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricOtelSdkLogCreated(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkLogCreated, descrMetricOtelSdkLogCreated,
                                    unitMetricOtelSdkLogCreated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkLogCreated(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkLogCreated, descrMetricOtelSdkLogCreated,
                                             unitMetricOtelSdkLogCreated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkLogCreated(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricOtelSdkLogCreated, descrMetricOtelSdkLogCreated, unitMetricOtelSdkLogCreated);
}

/**
  The duration of the collect operation of the metric reader.
  <p>
  For successful collections, @code error.type @endcode MUST NOT be set. For failed collections,
  @code error.type @endcode SHOULD contain the failure cause. It can happen that metrics collection
  is successful for some MetricProducers, while others fail. In that case @code error.type @endcode
  SHOULD be set to any of the failure causes. <p> histogram
 */
static constexpr const char *kMetricOtelSdkMetricReaderCollectionDuration =
    "otel.sdk.metric_reader.collection.duration";
static constexpr const char *descrMetricOtelSdkMetricReaderCollectionDuration =
    "The duration of the collect operation of the metric reader.";
static constexpr const char *unitMetricOtelSdkMetricReaderCollectionDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricOtelSdkMetricReaderCollectionDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricOtelSdkMetricReaderCollectionDuration,
                                      descrMetricOtelSdkMetricReaderCollectionDuration,
                                      unitMetricOtelSdkMetricReaderCollectionDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricOtelSdkMetricReaderCollectionDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricOtelSdkMetricReaderCollectionDuration,
                                      descrMetricOtelSdkMetricReaderCollectionDuration,
                                      unitMetricOtelSdkMetricReaderCollectionDuration);
}

/**
  The number of log records for which the processing has finished, either successful or failed.
  <p>
  For successful processing, @code error.type @endcode MUST NOT be set. For failed processing, @code
  error.type @endcode MUST contain the failure cause. For the SDK Simple and Batching Log Record
  Processor a log record is considered to be processed already when it has been submitted to the
  exporter, not when the corresponding export call has finished. <p> counter
 */
static constexpr const char *kMetricOtelSdkProcessorLogProcessed =
    "otel.sdk.processor.log.processed";
static constexpr const char *descrMetricOtelSdkProcessorLogProcessed =
    "The number of log records for which the processing has finished, either successful or failed.";
static constexpr const char *unitMetricOtelSdkProcessorLogProcessed = "{log_record}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkProcessorLogProcessed(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkProcessorLogProcessed,
                                    descrMetricOtelSdkProcessorLogProcessed,
                                    unitMetricOtelSdkProcessorLogProcessed);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkProcessorLogProcessed(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkProcessorLogProcessed,
                                    descrMetricOtelSdkProcessorLogProcessed,
                                    unitMetricOtelSdkProcessorLogProcessed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorLogProcessed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkProcessorLogProcessed,
                                             descrMetricOtelSdkProcessorLogProcessed,
                                             unitMetricOtelSdkProcessorLogProcessed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorLogProcessed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkProcessorLogProcessed,
                                              descrMetricOtelSdkProcessorLogProcessed,
                                              unitMetricOtelSdkProcessorLogProcessed);
}

/**
  The maximum number of log records the queue of a given instance of an SDK Log Record processor can
  hold. <p> Only applies to Log Record processors which use a queue, e.g. the SDK Batching Log
  Record Processor. <p> updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorLogQueueCapacity =
    "otel.sdk.processor.log.queue.capacity";
static constexpr const char *descrMetricOtelSdkProcessorLogQueueCapacity =
    "The maximum number of log records the queue of a given instance of an SDK Log Record "
    "processor can hold.";
static constexpr const char *unitMetricOtelSdkProcessorLogQueueCapacity = "{log_record}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkProcessorLogQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkProcessorLogQueueCapacity,
                                         descrMetricOtelSdkProcessorLogQueueCapacity,
                                         unitMetricOtelSdkProcessorLogQueueCapacity);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkProcessorLogQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkProcessorLogQueueCapacity,
                                          descrMetricOtelSdkProcessorLogQueueCapacity,
                                          unitMetricOtelSdkProcessorLogQueueCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorLogQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkProcessorLogQueueCapacity,
                                                   descrMetricOtelSdkProcessorLogQueueCapacity,
                                                   unitMetricOtelSdkProcessorLogQueueCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorLogQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkProcessorLogQueueCapacity,
                                                    descrMetricOtelSdkProcessorLogQueueCapacity,
                                                    unitMetricOtelSdkProcessorLogQueueCapacity);
}

/**
  The number of log records in the queue of a given instance of an SDK log processor.
  <p>
  Only applies to log record processors which use a queue, e.g. the SDK Batching Log Record
  Processor. <p> updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorLogQueueSize =
    "otel.sdk.processor.log.queue.size";
static constexpr const char *descrMetricOtelSdkProcessorLogQueueSize =
    "The number of log records in the queue of a given instance of an SDK log processor.";
static constexpr const char *unitMetricOtelSdkProcessorLogQueueSize = "{log_record}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkProcessorLogQueueSize(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkProcessorLogQueueSize,
                                         descrMetricOtelSdkProcessorLogQueueSize,
                                         unitMetricOtelSdkProcessorLogQueueSize);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkProcessorLogQueueSize(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkProcessorLogQueueSize,
                                          descrMetricOtelSdkProcessorLogQueueSize,
                                          unitMetricOtelSdkProcessorLogQueueSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorLogQueueSize(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkProcessorLogQueueSize,
                                                   descrMetricOtelSdkProcessorLogQueueSize,
                                                   unitMetricOtelSdkProcessorLogQueueSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorLogQueueSize(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkProcessorLogQueueSize,
                                                    descrMetricOtelSdkProcessorLogQueueSize,
                                                    unitMetricOtelSdkProcessorLogQueueSize);
}

/**
  The number of spans for which the processing has finished, either successful or failed.
  <p>
  For successful processing, @code error.type @endcode MUST NOT be set. For failed processing, @code
  error.type @endcode MUST contain the failure cause. For the SDK Simple and Batching Span Processor
  a span is considered to be processed already when it has been submitted to the exporter, not when
  the corresponding export call has finished. <p> counter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanProcessed =
    "otel.sdk.processor.span.processed";
static constexpr const char *descrMetricOtelSdkProcessorSpanProcessed =
    "The number of spans for which the processing has finished, either successful or failed.";
static constexpr const char *unitMetricOtelSdkProcessorSpanProcessed = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkProcessorSpanProcessed(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkProcessorSpanProcessed,
                                    descrMetricOtelSdkProcessorSpanProcessed,
                                    unitMetricOtelSdkProcessorSpanProcessed);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkProcessorSpanProcessed(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkProcessorSpanProcessed,
                                    descrMetricOtelSdkProcessorSpanProcessed,
                                    unitMetricOtelSdkProcessorSpanProcessed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorSpanProcessed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkProcessorSpanProcessed,
                                             descrMetricOtelSdkProcessorSpanProcessed,
                                             unitMetricOtelSdkProcessorSpanProcessed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorSpanProcessed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkProcessorSpanProcessed,
                                              descrMetricOtelSdkProcessorSpanProcessed,
                                              unitMetricOtelSdkProcessorSpanProcessed);
}

/**
  Deprecated, use @code otel.sdk.processor.span.processed @endcode instead.

  @deprecated
  {"note": "Replaced by @code otel.sdk.processor.span.processed @endcode.", "reason": "renamed",
  "renamed_to": "otel.sdk.processor.span.processed"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkProcessorSpanProcessedCount =
    "otel.sdk.processor.span.processed.count";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricOtelSdkProcessorSpanProcessedCount =
        "Deprecated, use `otel.sdk.processor.span.processed` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkProcessorSpanProcessedCount =
    "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                         descrMetricOtelSdkProcessorSpanProcessedCount,
                                         unitMetricOtelSdkProcessorSpanProcessedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                          descrMetricOtelSdkProcessorSpanProcessedCount,
                                          unitMetricOtelSdkProcessorSpanProcessedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                                   descrMetricOtelSdkProcessorSpanProcessedCount,
                                                   unitMetricOtelSdkProcessorSpanProcessedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                                    descrMetricOtelSdkProcessorSpanProcessedCount,
                                                    unitMetricOtelSdkProcessorSpanProcessedCount);
}

/**
  The maximum number of spans the queue of a given instance of an SDK span processor can hold.
  <p>
  Only applies to span processors which use a queue, e.g. the SDK Batching Span Processor.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanQueueCapacity =
    "otel.sdk.processor.span.queue.capacity";
static constexpr const char *descrMetricOtelSdkProcessorSpanQueueCapacity =
    "The maximum number of spans the queue of a given instance of an SDK span processor can hold.";
static constexpr const char *unitMetricOtelSdkProcessorSpanQueueCapacity = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkProcessorSpanQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkProcessorSpanQueueCapacity,
                                         descrMetricOtelSdkProcessorSpanQueueCapacity,
                                         unitMetricOtelSdkProcessorSpanQueueCapacity);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkProcessorSpanQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkProcessorSpanQueueCapacity,
                                          descrMetricOtelSdkProcessorSpanQueueCapacity,
                                          unitMetricOtelSdkProcessorSpanQueueCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorSpanQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkProcessorSpanQueueCapacity,
                                                   descrMetricOtelSdkProcessorSpanQueueCapacity,
                                                   unitMetricOtelSdkProcessorSpanQueueCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorSpanQueueCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkProcessorSpanQueueCapacity,
                                                    descrMetricOtelSdkProcessorSpanQueueCapacity,
                                                    unitMetricOtelSdkProcessorSpanQueueCapacity);
}

/**
  The number of spans in the queue of a given instance of an SDK span processor.
  <p>
  Only applies to span processors which use a queue, e.g. the SDK Batching Span Processor.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanQueueSize =
    "otel.sdk.processor.span.queue.size";
static constexpr const char *descrMetricOtelSdkProcessorSpanQueueSize =
    "The number of spans in the queue of a given instance of an SDK span processor.";
static constexpr const char *unitMetricOtelSdkProcessorSpanQueueSize = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkProcessorSpanQueueSize(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkProcessorSpanQueueSize,
                                         descrMetricOtelSdkProcessorSpanQueueSize,
                                         unitMetricOtelSdkProcessorSpanQueueSize);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkProcessorSpanQueueSize(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkProcessorSpanQueueSize,
                                          descrMetricOtelSdkProcessorSpanQueueSize,
                                          unitMetricOtelSdkProcessorSpanQueueSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorSpanQueueSize(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkProcessorSpanQueueSize,
                                                   descrMetricOtelSdkProcessorSpanQueueSize,
                                                   unitMetricOtelSdkProcessorSpanQueueSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorSpanQueueSize(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkProcessorSpanQueueSize,
                                                    descrMetricOtelSdkProcessorSpanQueueSize,
                                                    unitMetricOtelSdkProcessorSpanQueueSize);
}

/**
  Use @code otel.sdk.span.started @endcode minus @code otel.sdk.span.live @endcode to derive this
  value.

  @deprecated
  {"note": "Obsoleted.", "reason": "obsoleted"}
  <p>
  counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkSpanEnded =
    "otel.sdk.span.ended";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricOtelSdkSpanEnded =
    "Use `otel.sdk.span.started` minus `otel.sdk.span.live` to derive this value.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkSpanEnded = "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkSpanEnded(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkSpanEnded, descrMetricOtelSdkSpanEnded,
                                    unitMetricOtelSdkSpanEnded);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkSpanEnded(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkSpanEnded, descrMetricOtelSdkSpanEnded,
                                    unitMetricOtelSdkSpanEnded);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanEnded(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkSpanEnded, descrMetricOtelSdkSpanEnded,
                                             unitMetricOtelSdkSpanEnded);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanEnded(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkSpanEnded, descrMetricOtelSdkSpanEnded,
                                              unitMetricOtelSdkSpanEnded);
}

/**
  Use @code otel.sdk.span.started @endcode minus @code otel.sdk.span.live @endcode to derive this
  value.

  @deprecated
  {"note": "Obsoleted.", "reason": "obsoleted"}
  <p>
  counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkSpanEndedCount =
    "otel.sdk.span.ended.count";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricOtelSdkSpanEndedCount =
    "Use `otel.sdk.span.started` minus `otel.sdk.span.live` to derive this value.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkSpanEndedCount = "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkSpanEndedCount, descrMetricOtelSdkSpanEndedCount,
                                    unitMetricOtelSdkSpanEndedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkSpanEndedCount, descrMetricOtelSdkSpanEndedCount,
                                    unitMetricOtelSdkSpanEndedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkSpanEndedCount,
                                             descrMetricOtelSdkSpanEndedCount,
                                             unitMetricOtelSdkSpanEndedCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkSpanEndedCount,
                                              descrMetricOtelSdkSpanEndedCount,
                                              unitMetricOtelSdkSpanEndedCount);
}

/**
  The number of created spans with @code recording=true @endcode for which the end operation has not
  been called yet. <p> updowncounter
 */
static constexpr const char *kMetricOtelSdkSpanLive = "otel.sdk.span.live";
static constexpr const char *descrMetricOtelSdkSpanLive =
    "The number of created spans with `recording=true` for which the end operation has not been "
    "called yet.";
static constexpr const char *unitMetricOtelSdkSpanLive = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkSpanLive(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkSpanLive, descrMetricOtelSdkSpanLive,
                                         unitMetricOtelSdkSpanLive);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkSpanLive(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkSpanLive, descrMetricOtelSdkSpanLive,
                                          unitMetricOtelSdkSpanLive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanLive(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOtelSdkSpanLive, descrMetricOtelSdkSpanLive, unitMetricOtelSdkSpanLive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanLive(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOtelSdkSpanLive, descrMetricOtelSdkSpanLive, unitMetricOtelSdkSpanLive);
}

/**
  Deprecated, use @code otel.sdk.span.live @endcode instead.

  @deprecated
  {"note": "Replaced by @code otel.sdk.span.live @endcode.", "reason": "renamed", "renamed_to":
  "otel.sdk.span.live"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricOtelSdkSpanLiveCount =
    "otel.sdk.span.live.count";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricOtelSdkSpanLiveCount =
    "Deprecated, use `otel.sdk.span.live` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricOtelSdkSpanLiveCount = "{span}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

/**
  The number of created spans.
  <p>
  Implementations MUST record this metric for all spans, even for non-recording ones.
  <p>
  counter
 */
static constexpr const char *kMetricOtelSdkSpanStarted     = "otel.sdk.span.started";
static constexpr const char *descrMetricOtelSdkSpanStarted = "The number of created spans.";
static constexpr const char *unitMetricOtelSdkSpanStarted  = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricOtelSdkSpanStarted(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkSpanStarted, descrMetricOtelSdkSpanStarted,
                                    unitMetricOtelSdkSpanStarted);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricOtelSdkSpanStarted(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkSpanStarted, descrMetricOtelSdkSpanStarted,
                                    unitMetricOtelSdkSpanStarted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanStarted(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricOtelSdkSpanStarted, descrMetricOtelSdkSpanStarted, unitMetricOtelSdkSpanStarted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanStarted(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricOtelSdkSpanStarted, descrMetricOtelSdkSpanStarted, unitMetricOtelSdkSpanStarted);
}

}  // namespace otel
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
