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
namespace messaging
{

/**
 * Number of messages that were delivered to the application.
 * <p>
 * Records the number of messages pulled from the broker or number of messages dispatched to the
 * application in push-based scenarios. The metric SHOULD be reported once per message delivery. For
 * example, if receiving and processing operations are both instrumented for a single message
 * delivery, this counter is incremented when the message is received and not reported when it is
 * processed. <p> counter
 */
static constexpr const char *kMetricMessagingClientConsumedMessages =
    "messaging.client.consumed.messages";
static constexpr const char *descrMetricMessagingClientConsumedMessages =
    "Number of messages that were delivered to the application.";
static constexpr const char *unitMetricMessagingClientConsumedMessages = "{message}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingClientConsumedMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingClientConsumedMessages,
                                    descrMetricMessagingClientConsumedMessages,
                                    unitMetricMessagingClientConsumedMessages);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingClientConsumedMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingClientConsumedMessages,
                                    descrMetricMessagingClientConsumedMessages,
                                    unitMetricMessagingClientConsumedMessages);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingClientConsumedMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingClientConsumedMessages,
                                             descrMetricMessagingClientConsumedMessages,
                                             unitMetricMessagingClientConsumedMessages);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingClientConsumedMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingClientConsumedMessages,
                                              descrMetricMessagingClientConsumedMessages,
                                              unitMetricMessagingClientConsumedMessages);
}

/**
 * Duration of messaging operation initiated by a producer or consumer client.
 * <p>
 * This metric SHOULD NOT be used to report processing duration - processing duration is reported in
 * @code messaging.process.duration @endcode metric. <p> histogram
 */
static constexpr const char *kMetricMessagingClientOperationDuration =
    "messaging.client.operation.duration";
static constexpr const char *descrMetricMessagingClientOperationDuration =
    "Duration of messaging operation initiated by a producer or consumer client.";
static constexpr const char *unitMetricMessagingClientOperationDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricMessagingClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricMessagingClientOperationDuration,
                                      descrMetricMessagingClientOperationDuration,
                                      unitMetricMessagingClientOperationDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricMessagingClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricMessagingClientOperationDuration,
                                      descrMetricMessagingClientOperationDuration,
                                      unitMetricMessagingClientOperationDuration);
}

/**
 * Deprecated. Use @code messaging.client.sent.messages @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.sent.messages @endcode.", "reason": "uncategorized"}
 * <p>
 * counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingClientPublishedMessages =
    "messaging.client.published.messages";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingClientPublishedMessages =
    "Deprecated. Use `messaging.client.sent.messages` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingClientPublishedMessages =
    "{message}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingClientPublishedMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingClientPublishedMessages,
                                    descrMetricMessagingClientPublishedMessages,
                                    unitMetricMessagingClientPublishedMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingClientPublishedMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingClientPublishedMessages,
                                    descrMetricMessagingClientPublishedMessages,
                                    unitMetricMessagingClientPublishedMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingClientPublishedMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingClientPublishedMessages,
                                             descrMetricMessagingClientPublishedMessages,
                                             unitMetricMessagingClientPublishedMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingClientPublishedMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingClientPublishedMessages,
                                              descrMetricMessagingClientPublishedMessages,
                                              unitMetricMessagingClientPublishedMessages);
}

/**
 * Number of messages producer attempted to send to the broker.
 * <p>
 * This metric MUST NOT count messages that were created but haven't yet been sent.
 * <p>
 * counter
 */
static constexpr const char *kMetricMessagingClientSentMessages = "messaging.client.sent.messages";
static constexpr const char *descrMetricMessagingClientSentMessages =
    "Number of messages producer attempted to send to the broker.";
static constexpr const char *unitMetricMessagingClientSentMessages = "{message}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingClientSentMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingClientSentMessages,
                                    descrMetricMessagingClientSentMessages,
                                    unitMetricMessagingClientSentMessages);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingClientSentMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingClientSentMessages,
                                    descrMetricMessagingClientSentMessages,
                                    unitMetricMessagingClientSentMessages);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingClientSentMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingClientSentMessages,
                                             descrMetricMessagingClientSentMessages,
                                             unitMetricMessagingClientSentMessages);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingClientSentMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingClientSentMessages,
                                              descrMetricMessagingClientSentMessages,
                                              unitMetricMessagingClientSentMessages);
}

/**
 * Duration of processing operation.
 * <p>
 * This metric MUST be reported for operations with @code messaging.operation.type @endcode that
 * matches @code process @endcode. <p> histogram
 */
static constexpr const char *kMetricMessagingProcessDuration = "messaging.process.duration";
static constexpr const char *descrMetricMessagingProcessDuration =
    "Duration of processing operation.";
static constexpr const char *unitMetricMessagingProcessDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricMessagingProcessDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricMessagingProcessDuration,
                                      descrMetricMessagingProcessDuration,
                                      unitMetricMessagingProcessDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricMessagingProcessDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricMessagingProcessDuration,
                                      descrMetricMessagingProcessDuration,
                                      unitMetricMessagingProcessDuration);
}

/**
 * Deprecated. Use @code messaging.client.consumed.messages @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.consumed.messages @endcode.", "reason":
 * "uncategorized"} <p> counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingProcessMessages =
    "messaging.process.messages";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingProcessMessages =
    "Deprecated. Use `messaging.client.consumed.messages` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingProcessMessages =
    "{message}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingProcessMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingProcessMessages,
                                    descrMetricMessagingProcessMessages,
                                    unitMetricMessagingProcessMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingProcessMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingProcessMessages,
                                    descrMetricMessagingProcessMessages,
                                    unitMetricMessagingProcessMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingProcessMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingProcessMessages,
                                             descrMetricMessagingProcessMessages,
                                             unitMetricMessagingProcessMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingProcessMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingProcessMessages,
                                              descrMetricMessagingProcessMessages,
                                              unitMetricMessagingProcessMessages);
}

/**
 * Deprecated. Use @code messaging.client.operation.duration @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.operation.duration @endcode.", "reason":
 * "uncategorized"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingPublishDuration =
    "messaging.publish.duration";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingPublishDuration =
    "Deprecated. Use `messaging.client.operation.duration` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingPublishDuration = "s";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricMessagingPublishDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricMessagingPublishDuration,
                                      descrMetricMessagingPublishDuration,
                                      unitMetricMessagingPublishDuration);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricMessagingPublishDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricMessagingPublishDuration,
                                      descrMetricMessagingPublishDuration,
                                      unitMetricMessagingPublishDuration);
}

/**
 * Deprecated. Use @code messaging.client.produced.messages @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.produced.messages @endcode.", "reason":
 * "uncategorized"} <p> counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingPublishMessages =
    "messaging.publish.messages";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingPublishMessages =
    "Deprecated. Use `messaging.client.produced.messages` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingPublishMessages =
    "{message}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingPublishMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingPublishMessages,
                                    descrMetricMessagingPublishMessages,
                                    unitMetricMessagingPublishMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingPublishMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingPublishMessages,
                                    descrMetricMessagingPublishMessages,
                                    unitMetricMessagingPublishMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingPublishMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingPublishMessages,
                                             descrMetricMessagingPublishMessages,
                                             unitMetricMessagingPublishMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingPublishMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingPublishMessages,
                                              descrMetricMessagingPublishMessages,
                                              unitMetricMessagingPublishMessages);
}

/**
 * Deprecated. Use @code messaging.client.operation.duration @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.operation.duration @endcode.", "reason":
 * "uncategorized"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingReceiveDuration =
    "messaging.receive.duration";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingReceiveDuration =
    "Deprecated. Use `messaging.client.operation.duration` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingReceiveDuration = "s";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricMessagingReceiveDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricMessagingReceiveDuration,
                                      descrMetricMessagingReceiveDuration,
                                      unitMetricMessagingReceiveDuration);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricMessagingReceiveDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricMessagingReceiveDuration,
                                      descrMetricMessagingReceiveDuration,
                                      unitMetricMessagingReceiveDuration);
}

/**
 * Deprecated. Use @code messaging.client.consumed.messages @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code messaging.client.consumed.messages @endcode.", "reason":
 * "uncategorized"} <p> counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricMessagingReceiveMessages =
    "messaging.receive.messages";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricMessagingReceiveMessages =
    "Deprecated. Use `messaging.client.consumed.messages` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricMessagingReceiveMessages =
    "{message}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricMessagingReceiveMessages(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricMessagingReceiveMessages,
                                    descrMetricMessagingReceiveMessages,
                                    unitMetricMessagingReceiveMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricMessagingReceiveMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricMessagingReceiveMessages,
                                    descrMetricMessagingReceiveMessages,
                                    unitMetricMessagingReceiveMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricMessagingReceiveMessages(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricMessagingReceiveMessages,
                                             descrMetricMessagingReceiveMessages,
                                             unitMetricMessagingReceiveMessages);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricMessagingReceiveMessages(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricMessagingReceiveMessages,
                                              descrMetricMessagingReceiveMessages,
                                              unitMetricMessagingReceiveMessages);
}

}  // namespace messaging
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
