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
 * The number of spans for which the export has finished, either successful or failed
 * <p>
 * For successful exports, @code error.type @endcode MUST NOT be set. For failed exports, @code
 * error.type @endcode must contain the failure cause. For exporters with partial success semantics
 * (e.g. OTLP with @code rejected_spans @endcode), rejected spans must count as failed and only
 * non-rejected spans count as success. If no rejection reason is available, @code rejected @endcode
 * SHOULD be used as value for @code error.type @endcode. <p> counter
 */
static constexpr const char *kMetricOtelSdkExporterSpanExportedCount =
    "otel.sdk.exporter.span.exported.count";
static constexpr const char *descrMetricOtelSdkExporterSpanExportedCount =
    "The number of spans for which the export has finished, either successful or failed";
static constexpr const char *unitMetricOtelSdkExporterSpanExportedCount = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkExporterSpanExportedCount,
                                    descrMetricOtelSdkExporterSpanExportedCount,
                                    unitMetricOtelSdkExporterSpanExportedCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkExporterSpanExportedCount,
                                    descrMetricOtelSdkExporterSpanExportedCount,
                                    unitMetricOtelSdkExporterSpanExportedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkExporterSpanExportedCount,
                                             descrMetricOtelSdkExporterSpanExportedCount,
                                             unitMetricOtelSdkExporterSpanExportedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanExportedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkExporterSpanExportedCount,
                                              descrMetricOtelSdkExporterSpanExportedCount,
                                              unitMetricOtelSdkExporterSpanExportedCount);
}

/**
 * The number of spans which were passed to the exporter, but that have not been exported yet
 * (neither successful, nor failed) <p> For successful exports, @code error.type @endcode MUST NOT
 * be set. For failed exports, @code error.type @endcode must contain the failure cause. <p>
 * updowncounter
 */
static constexpr const char *kMetricOtelSdkExporterSpanInflightCount =
    "otel.sdk.exporter.span.inflight.count";
static constexpr const char *descrMetricOtelSdkExporterSpanInflightCount =
    "The number of spans which were passed to the exporter, but that have not been exported yet "
    "(neither successful, nor failed)";
static constexpr const char *unitMetricOtelSdkExporterSpanInflightCount = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                         descrMetricOtelSdkExporterSpanInflightCount,
                                         unitMetricOtelSdkExporterSpanInflightCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                          descrMetricOtelSdkExporterSpanInflightCount,
                                          unitMetricOtelSdkExporterSpanInflightCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                                   descrMetricOtelSdkExporterSpanInflightCount,
                                                   unitMetricOtelSdkExporterSpanInflightCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkExporterSpanInflightCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOtelSdkExporterSpanInflightCount,
                                                    descrMetricOtelSdkExporterSpanInflightCount,
                                                    unitMetricOtelSdkExporterSpanInflightCount);
}

/**
 * The number of spans for which the processing has finished, either successful or failed
 * <p>
 * For successful processing, @code error.type @endcode MUST NOT be set. For failed processing,
 * @code error.type @endcode must contain the failure cause. For the SDK Simple and Batching Span
 * Processor a span is considered to be processed already when it has been submitted to the
 * exporter, not when the corresponding export call has finished. <p> counter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanProcessedCount =
    "otel.sdk.processor.span.processed.count";
static constexpr const char *descrMetricOtelSdkProcessorSpanProcessedCount =
    "The number of spans for which the processing has finished, either successful or failed";
static constexpr const char *unitMetricOtelSdkProcessorSpanProcessedCount = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkProcessorSpanProcessedCount,
                                    descrMetricOtelSdkProcessorSpanProcessedCount,
                                    unitMetricOtelSdkProcessorSpanProcessedCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                    descrMetricOtelSdkProcessorSpanProcessedCount,
                                    unitMetricOtelSdkProcessorSpanProcessedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                             descrMetricOtelSdkProcessorSpanProcessedCount,
                                             unitMetricOtelSdkProcessorSpanProcessedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkProcessorSpanProcessedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkProcessorSpanProcessedCount,
                                              descrMetricOtelSdkProcessorSpanProcessedCount,
                                              unitMetricOtelSdkProcessorSpanProcessedCount);
}

/**
 * The maximum number of spans the queue of a given instance of an SDK span processor can hold
 * <p>
 * Only applies to span processors which use a queue, e.g. the SDK Batching Span Processor.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanQueueCapacity =
    "otel.sdk.processor.span.queue.capacity";
static constexpr const char *descrMetricOtelSdkProcessorSpanQueueCapacity =
    "The maximum number of spans the queue of a given instance of an SDK span processor can hold";
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
 * The number of spans in the queue of a given instance of an SDK span processor
 * <p>
 * Only applies to span processors which use a queue, e.g. the SDK Batching Span Processor.
 * <p>
 * updowncounter
 */
static constexpr const char *kMetricOtelSdkProcessorSpanQueueSize =
    "otel.sdk.processor.span.queue.size";
static constexpr const char *descrMetricOtelSdkProcessorSpanQueueSize =
    "The number of spans in the queue of a given instance of an SDK span processor";
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
 * The number of created spans for which the end operation was called
 * <p>
 * For spans with @code recording=true @endcode: Implementations MUST record both @code
 * otel.sdk.span.live.count @endcode and @code otel.sdk.span.ended.count @endcode. For spans with
 * @code recording=false @endcode: If implementations decide to record this metric, they MUST also
 * record @code otel.sdk.span.live.count @endcode. <p> counter
 */
static constexpr const char *kMetricOtelSdkSpanEndedCount = "otel.sdk.span.ended.count";
static constexpr const char *descrMetricOtelSdkSpanEndedCount =
    "The number of created spans for which the end operation was called";
static constexpr const char *unitMetricOtelSdkSpanEndedCount = "{span}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricOtelSdkSpanEndedCount, descrMetricOtelSdkSpanEndedCount,
                                    unitMetricOtelSdkSpanEndedCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricOtelSdkSpanEndedCount, descrMetricOtelSdkSpanEndedCount,
                                    unitMetricOtelSdkSpanEndedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricOtelSdkSpanEndedCount,
                                             descrMetricOtelSdkSpanEndedCount,
                                             unitMetricOtelSdkSpanEndedCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanEndedCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricOtelSdkSpanEndedCount,
                                              descrMetricOtelSdkSpanEndedCount,
                                              unitMetricOtelSdkSpanEndedCount);
}

/**
 * The number of created spans for which the end operation has not been called yet
 * <p>
 * For spans with @code recording=true @endcode: Implementations MUST record both @code
 * otel.sdk.span.live.count @endcode and @code otel.sdk.span.ended.count @endcode. For spans with
 * @code recording=false @endcode: If implementations decide to record this metric, they MUST also
 * record @code otel.sdk.span.ended.count @endcode. <p> updowncounter
 */
static constexpr const char *kMetricOtelSdkSpanLiveCount = "otel.sdk.span.live.count";
static constexpr const char *descrMetricOtelSdkSpanLiveCount =
    "The number of created spans for which the end operation has not been called yet";
static constexpr const char *unitMetricOtelSdkSpanLiveCount = "{span}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOtelSdkSpanLiveCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOtelSdkSpanLiveCount, descrMetricOtelSdkSpanLiveCount, unitMetricOtelSdkSpanLiveCount);
}

}  // namespace otel
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
