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
namespace db
{

/**
  The number of connections that are currently in state described by the @code state @endcode
  attribute. <p> updowncounter
 */
static constexpr const char *kMetricDbClientConnectionCount = "db.client.connection.count";
static constexpr const char *descrMetricDbClientConnectionCount =
    "The number of connections that are currently in state described by the `state` attribute.";
static constexpr const char *unitMetricDbClientConnectionCount = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionCount,
                                         descrMetricDbClientConnectionCount,
                                         unitMetricDbClientConnectionCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionCount,
                                          descrMetricDbClientConnectionCount,
                                          unitMetricDbClientConnectionCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionCount,
                                                   descrMetricDbClientConnectionCount,
                                                   unitMetricDbClientConnectionCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionCount,
                                                    descrMetricDbClientConnectionCount,
                                                    unitMetricDbClientConnectionCount);
}

/**
  The time it took to create a new connection.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientConnectionCreateTime =
    "db.client.connection.create_time";
static constexpr const char *descrMetricDbClientConnectionCreateTime =
    "The time it took to create a new connection.";
static constexpr const char *unitMetricDbClientConnectionCreateTime = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionCreateTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionCreateTime,
                                      descrMetricDbClientConnectionCreateTime,
                                      unitMetricDbClientConnectionCreateTime);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionCreateTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionCreateTime,
                                      descrMetricDbClientConnectionCreateTime,
                                      unitMetricDbClientConnectionCreateTime);
}

/**
  The maximum number of idle open connections allowed.
  <p>
  updowncounter
 */
static constexpr const char *kMetricDbClientConnectionIdleMax = "db.client.connection.idle.max";
static constexpr const char *descrMetricDbClientConnectionIdleMax =
    "The maximum number of idle open connections allowed.";
static constexpr const char *unitMetricDbClientConnectionIdleMax = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionIdleMax(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionIdleMax,
                                         descrMetricDbClientConnectionIdleMax,
                                         unitMetricDbClientConnectionIdleMax);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionIdleMax(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionIdleMax,
                                          descrMetricDbClientConnectionIdleMax,
                                          unitMetricDbClientConnectionIdleMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionIdleMax(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionIdleMax,
                                                   descrMetricDbClientConnectionIdleMax,
                                                   unitMetricDbClientConnectionIdleMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionIdleMax(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionIdleMax,
                                                    descrMetricDbClientConnectionIdleMax,
                                                    unitMetricDbClientConnectionIdleMax);
}

/**
  The minimum number of idle open connections allowed.
  <p>
  updowncounter
 */
static constexpr const char *kMetricDbClientConnectionIdleMin = "db.client.connection.idle.min";
static constexpr const char *descrMetricDbClientConnectionIdleMin =
    "The minimum number of idle open connections allowed.";
static constexpr const char *unitMetricDbClientConnectionIdleMin = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionIdleMin(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionIdleMin,
                                         descrMetricDbClientConnectionIdleMin,
                                         unitMetricDbClientConnectionIdleMin);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionIdleMin(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionIdleMin,
                                          descrMetricDbClientConnectionIdleMin,
                                          unitMetricDbClientConnectionIdleMin);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionIdleMin(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionIdleMin,
                                                   descrMetricDbClientConnectionIdleMin,
                                                   unitMetricDbClientConnectionIdleMin);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionIdleMin(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionIdleMin,
                                                    descrMetricDbClientConnectionIdleMin,
                                                    unitMetricDbClientConnectionIdleMin);
}

/**
  The maximum number of open connections allowed.
  <p>
  updowncounter
 */
static constexpr const char *kMetricDbClientConnectionMax = "db.client.connection.max";
static constexpr const char *descrMetricDbClientConnectionMax =
    "The maximum number of open connections allowed.";
static constexpr const char *unitMetricDbClientConnectionMax = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionMax(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionMax,
                                         descrMetricDbClientConnectionMax,
                                         unitMetricDbClientConnectionMax);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionMax(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionMax,
                                          descrMetricDbClientConnectionMax,
                                          unitMetricDbClientConnectionMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionMax(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionMax,
                                                   descrMetricDbClientConnectionMax,
                                                   unitMetricDbClientConnectionMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionMax(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionMax,
                                                    descrMetricDbClientConnectionMax,
                                                    unitMetricDbClientConnectionMax);
}

/**
  The number of current pending requests for an open connection.
  <p>
  updowncounter
 */
static constexpr const char *kMetricDbClientConnectionPendingRequests =
    "db.client.connection.pending_requests";
static constexpr const char *descrMetricDbClientConnectionPendingRequests =
    "The number of current pending requests for an open connection.";
static constexpr const char *unitMetricDbClientConnectionPendingRequests = "{request}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionPendingRequests(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionPendingRequests,
                                         descrMetricDbClientConnectionPendingRequests,
                                         unitMetricDbClientConnectionPendingRequests);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionPendingRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionPendingRequests,
                                          descrMetricDbClientConnectionPendingRequests,
                                          unitMetricDbClientConnectionPendingRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionPendingRequests(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionPendingRequests,
                                                   descrMetricDbClientConnectionPendingRequests,
                                                   unitMetricDbClientConnectionPendingRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionPendingRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionPendingRequests,
                                                    descrMetricDbClientConnectionPendingRequests,
                                                    unitMetricDbClientConnectionPendingRequests);
}

/**
  The number of connection timeouts that have occurred trying to obtain a connection from the pool.
  <p>
  counter
 */
static constexpr const char *kMetricDbClientConnectionTimeouts = "db.client.connection.timeouts";
static constexpr const char *descrMetricDbClientConnectionTimeouts =
    "The number of connection timeouts that have occurred trying to obtain a connection from the "
    "pool.";
static constexpr const char *unitMetricDbClientConnectionTimeouts = "{timeout}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricDbClientConnectionTimeouts(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricDbClientConnectionTimeouts,
                                    descrMetricDbClientConnectionTimeouts,
                                    unitMetricDbClientConnectionTimeouts);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricDbClientConnectionTimeouts(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricDbClientConnectionTimeouts,
                                    descrMetricDbClientConnectionTimeouts,
                                    unitMetricDbClientConnectionTimeouts);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionTimeouts(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricDbClientConnectionTimeouts,
                                             descrMetricDbClientConnectionTimeouts,
                                             unitMetricDbClientConnectionTimeouts);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionTimeouts(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricDbClientConnectionTimeouts,
                                              descrMetricDbClientConnectionTimeouts,
                                              unitMetricDbClientConnectionTimeouts);
}

/**
  The time between borrowing a connection and returning it to the pool.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientConnectionUseTime = "db.client.connection.use_time";
static constexpr const char *descrMetricDbClientConnectionUseTime =
    "The time between borrowing a connection and returning it to the pool.";
static constexpr const char *unitMetricDbClientConnectionUseTime = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionUseTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionUseTime,
                                      descrMetricDbClientConnectionUseTime,
                                      unitMetricDbClientConnectionUseTime);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionUseTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionUseTime,
                                      descrMetricDbClientConnectionUseTime,
                                      unitMetricDbClientConnectionUseTime);
}

/**
  The time it took to obtain an open connection from the pool.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientConnectionWaitTime = "db.client.connection.wait_time";
static constexpr const char *descrMetricDbClientConnectionWaitTime =
    "The time it took to obtain an open connection from the pool.";
static constexpr const char *unitMetricDbClientConnectionWaitTime = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionWaitTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionWaitTime,
                                      descrMetricDbClientConnectionWaitTime,
                                      unitMetricDbClientConnectionWaitTime);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionWaitTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionWaitTime,
                                      descrMetricDbClientConnectionWaitTime,
                                      unitMetricDbClientConnectionWaitTime);
}

/**
  Deprecated, use @code db.client.connection.create_time @endcode instead. Note: the unit also
  changed from @code ms @endcode to @code s @endcode.

  @deprecated
  {"note": "Replaced by @code db.client.connection.create_time @endcode with unit @code s
  @endcode.", "reason": "uncategorized"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsCreateTime =
    "db.client.connections.create_time";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsCreateTime =
    "Deprecated, use `db.client.connection.create_time` instead. Note: the unit also changed from "
    "`ms` to `s`.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsCreateTime =
    "ms";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionsCreateTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionsCreateTime,
                                      descrMetricDbClientConnectionsCreateTime,
                                      unitMetricDbClientConnectionsCreateTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionsCreateTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionsCreateTime,
                                      descrMetricDbClientConnectionsCreateTime,
                                      unitMetricDbClientConnectionsCreateTime);
}

/**
  Deprecated, use @code db.client.connection.idle.max @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.idle.max @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.idle.max"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsIdleMax =
    "db.client.connections.idle.max";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsIdleMax =
    "Deprecated, use `db.client.connection.idle.max` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsIdleMax =
    "{connection}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionsIdleMax(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionsIdleMax,
                                         descrMetricDbClientConnectionsIdleMax,
                                         unitMetricDbClientConnectionsIdleMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionsIdleMax(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionsIdleMax,
                                          descrMetricDbClientConnectionsIdleMax,
                                          unitMetricDbClientConnectionsIdleMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsIdleMax(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionsIdleMax,
                                                   descrMetricDbClientConnectionsIdleMax,
                                                   unitMetricDbClientConnectionsIdleMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsIdleMax(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionsIdleMax,
                                                    descrMetricDbClientConnectionsIdleMax,
                                                    unitMetricDbClientConnectionsIdleMax);
}

/**
  Deprecated, use @code db.client.connection.idle.min @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.idle.min @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.idle.min"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsIdleMin =
    "db.client.connections.idle.min";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsIdleMin =
    "Deprecated, use `db.client.connection.idle.min` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsIdleMin =
    "{connection}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionsIdleMin(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionsIdleMin,
                                         descrMetricDbClientConnectionsIdleMin,
                                         unitMetricDbClientConnectionsIdleMin);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionsIdleMin(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionsIdleMin,
                                          descrMetricDbClientConnectionsIdleMin,
                                          unitMetricDbClientConnectionsIdleMin);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsIdleMin(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionsIdleMin,
                                                   descrMetricDbClientConnectionsIdleMin,
                                                   unitMetricDbClientConnectionsIdleMin);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsIdleMin(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionsIdleMin,
                                                    descrMetricDbClientConnectionsIdleMin,
                                                    unitMetricDbClientConnectionsIdleMin);
}

/**
  Deprecated, use @code db.client.connection.max @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.max @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.max"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsMax =
    "db.client.connections.max";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsMax =
    "Deprecated, use `db.client.connection.max` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsMax =
    "{connection}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionsMax(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionsMax,
                                         descrMetricDbClientConnectionsMax,
                                         unitMetricDbClientConnectionsMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionsMax(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionsMax,
                                          descrMetricDbClientConnectionsMax,
                                          unitMetricDbClientConnectionsMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsMax(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionsMax,
                                                   descrMetricDbClientConnectionsMax,
                                                   unitMetricDbClientConnectionsMax);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsMax(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionsMax,
                                                    descrMetricDbClientConnectionsMax,
                                                    unitMetricDbClientConnectionsMax);
}

/**
  Deprecated, use @code db.client.connection.pending_requests @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.pending_requests @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.pending_requests"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsPendingRequests =
    "db.client.connections.pending_requests";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricDbClientConnectionsPendingRequests =
        "Deprecated, use `db.client.connection.pending_requests` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsPendingRequests =
    "{request}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionsPendingRequests(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionsPendingRequests,
                                         descrMetricDbClientConnectionsPendingRequests,
                                         unitMetricDbClientConnectionsPendingRequests);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionsPendingRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionsPendingRequests,
                                          descrMetricDbClientConnectionsPendingRequests,
                                          unitMetricDbClientConnectionsPendingRequests);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsPendingRequests(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionsPendingRequests,
                                                   descrMetricDbClientConnectionsPendingRequests,
                                                   unitMetricDbClientConnectionsPendingRequests);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsPendingRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionsPendingRequests,
                                                    descrMetricDbClientConnectionsPendingRequests,
                                                    unitMetricDbClientConnectionsPendingRequests);
}

/**
  Deprecated, use @code db.client.connection.timeouts @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.timeouts @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.timeouts"} <p> counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsTimeouts =
    "db.client.connections.timeouts";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsTimeouts =
    "Deprecated, use `db.client.connection.timeouts` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsTimeouts =
    "{timeout}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricDbClientConnectionsTimeouts(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricDbClientConnectionsTimeouts,
                                    descrMetricDbClientConnectionsTimeouts,
                                    unitMetricDbClientConnectionsTimeouts);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricDbClientConnectionsTimeouts(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricDbClientConnectionsTimeouts,
                                    descrMetricDbClientConnectionsTimeouts,
                                    unitMetricDbClientConnectionsTimeouts);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsTimeouts(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricDbClientConnectionsTimeouts,
                                             descrMetricDbClientConnectionsTimeouts,
                                             unitMetricDbClientConnectionsTimeouts);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsTimeouts(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricDbClientConnectionsTimeouts,
                                              descrMetricDbClientConnectionsTimeouts,
                                              unitMetricDbClientConnectionsTimeouts);
}

/**
  Deprecated, use @code db.client.connection.count @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.count @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.count"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsUsage =
    "db.client.connections.usage";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsUsage =
    "Deprecated, use `db.client.connection.count` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsUsage =
    "{connection}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientConnectionsUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientConnectionsUsage,
                                         descrMetricDbClientConnectionsUsage,
                                         unitMetricDbClientConnectionsUsage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientConnectionsUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientConnectionsUsage,
                                          descrMetricDbClientConnectionsUsage,
                                          unitMetricDbClientConnectionsUsage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientConnectionsUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientConnectionsUsage,
                                                   descrMetricDbClientConnectionsUsage,
                                                   unitMetricDbClientConnectionsUsage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientConnectionsUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientConnectionsUsage,
                                                    descrMetricDbClientConnectionsUsage,
                                                    unitMetricDbClientConnectionsUsage);
}

/**
  Deprecated, use @code db.client.connection.use_time @endcode instead. Note: the unit also changed
  from @code ms @endcode to @code s @endcode.

  @deprecated
  {"note": "Replaced by @code db.client.connection.use_time @endcode with unit @code s @endcode.",
  "reason": "uncategorized"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsUseTime =
    "db.client.connections.use_time";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsUseTime =
    "Deprecated, use `db.client.connection.use_time` instead. Note: the unit also changed from "
    "`ms` to `s`.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsUseTime = "ms";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionsUseTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionsUseTime,
                                      descrMetricDbClientConnectionsUseTime,
                                      unitMetricDbClientConnectionsUseTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionsUseTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionsUseTime,
                                      descrMetricDbClientConnectionsUseTime,
                                      unitMetricDbClientConnectionsUseTime);
}

/**
  Deprecated, use @code db.client.connection.wait_time @endcode instead. Note: the unit also changed
  from @code ms @endcode to @code s @endcode.

  @deprecated
  {"note": "Replaced by @code db.client.connection.wait_time @endcode with unit @code s @endcode.",
  "reason": "uncategorized"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientConnectionsWaitTime =
    "db.client.connections.wait_time";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricDbClientConnectionsWaitTime =
    "Deprecated, use `db.client.connection.wait_time` instead. Note: the unit also changed from "
    "`ms` to `s`.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricDbClientConnectionsWaitTime = "ms";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientConnectionsWaitTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientConnectionsWaitTime,
                                      descrMetricDbClientConnectionsWaitTime,
                                      unitMetricDbClientConnectionsWaitTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientConnectionsWaitTime(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientConnectionsWaitTime,
                                      descrMetricDbClientConnectionsWaitTime,
                                      unitMetricDbClientConnectionsWaitTime);
}

/**
  Deprecated, use @code azure.cosmosdb.client.active_instance.count @endcode instead.

  @deprecated
  {"note": "Replaced by @code azure.cosmosdb.client.active_instance.count @endcode.", "reason":
  "renamed", "renamed_to": "azure.cosmosdb.client.active_instance.count"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricDbClientCosmosdbActiveInstanceCount =
    "db.client.cosmosdb.active_instance.count";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricDbClientCosmosdbActiveInstanceCount =
        "Deprecated, use `azure.cosmosdb.client.active_instance.count` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricDbClientCosmosdbActiveInstanceCount = "{instance}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricDbClientCosmosdbActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricDbClientCosmosdbActiveInstanceCount,
                                         descrMetricDbClientCosmosdbActiveInstanceCount,
                                         unitMetricDbClientCosmosdbActiveInstanceCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricDbClientCosmosdbActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricDbClientCosmosdbActiveInstanceCount,
                                          descrMetricDbClientCosmosdbActiveInstanceCount,
                                          unitMetricDbClientCosmosdbActiveInstanceCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricDbClientCosmosdbActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricDbClientCosmosdbActiveInstanceCount,
                                                   descrMetricDbClientCosmosdbActiveInstanceCount,
                                                   unitMetricDbClientCosmosdbActiveInstanceCount);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricDbClientCosmosdbActiveInstanceCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricDbClientCosmosdbActiveInstanceCount,
                                                    descrMetricDbClientCosmosdbActiveInstanceCount,
                                                    unitMetricDbClientCosmosdbActiveInstanceCount);
}

/**
  Deprecated, use @code azure.cosmosdb.client.operation.request_charge @endcode instead.

  @deprecated
  {"note": "Replaced by @code azure.cosmosdb.client.operation.request_charge @endcode.", "reason":
  "renamed", "renamed_to": "azure.cosmosdb.client.operation.request_charge"} <p> histogram
 */
OPENTELEMETRY_DEPRECATED static constexpr const char
    *kMetricDbClientCosmosdbOperationRequestCharge = "db.client.cosmosdb.operation.request_charge";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricDbClientCosmosdbOperationRequestCharge =
        "Deprecated, use `azure.cosmosdb.client.operation.request_charge` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricDbClientCosmosdbOperationRequestCharge = "{request_unit}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientCosmosdbOperationRequestCharge(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientCosmosdbOperationRequestCharge,
                                      descrMetricDbClientCosmosdbOperationRequestCharge,
                                      unitMetricDbClientCosmosdbOperationRequestCharge);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientCosmosdbOperationRequestCharge(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientCosmosdbOperationRequestCharge,
                                      descrMetricDbClientCosmosdbOperationRequestCharge,
                                      unitMetricDbClientCosmosdbOperationRequestCharge);
}

/**
  Duration of database client operations.
  <p>
  Batch operations SHOULD be recorded as a single operation.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientOperationDuration = "db.client.operation.duration";
static constexpr const char *descrMetricDbClientOperationDuration =
    "Duration of database client operations.";
static constexpr const char *unitMetricDbClientOperationDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientOperationDuration,
                                      descrMetricDbClientOperationDuration,
                                      unitMetricDbClientOperationDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientOperationDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientOperationDuration,
                                      descrMetricDbClientOperationDuration,
                                      unitMetricDbClientOperationDuration);
}

/**
  The actual number of records returned by the database operation.
  <p>
  histogram
 */
static constexpr const char *kMetricDbClientResponseReturnedRows =
    "db.client.response.returned_rows";
static constexpr const char *descrMetricDbClientResponseReturnedRows =
    "The actual number of records returned by the database operation.";
static constexpr const char *unitMetricDbClientResponseReturnedRows = "{row}";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricDbClientResponseReturnedRows(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricDbClientResponseReturnedRows,
                                      descrMetricDbClientResponseReturnedRows,
                                      unitMetricDbClientResponseReturnedRows);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricDbClientResponseReturnedRows(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricDbClientResponseReturnedRows,
                                      descrMetricDbClientResponseReturnedRows,
                                      unitMetricDbClientResponseReturnedRows);
}

}  // namespace db
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
