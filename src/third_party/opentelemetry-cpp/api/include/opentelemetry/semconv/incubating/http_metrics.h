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
namespace http
{

/**
  Number of active HTTP requests.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHttpClientActiveRequests = "http.client.active_requests";
static constexpr const char *descrMetricHttpClientActiveRequests =
    "Number of active HTTP requests.";
static constexpr const char *unitMetricHttpClientActiveRequests = "{request}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHttpClientActiveRequests(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHttpClientActiveRequests,
                                         descrMetricHttpClientActiveRequests,
                                         unitMetricHttpClientActiveRequests);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHttpClientActiveRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHttpClientActiveRequests,
                                          descrMetricHttpClientActiveRequests,
                                          unitMetricHttpClientActiveRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHttpClientActiveRequests(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHttpClientActiveRequests,
                                                   descrMetricHttpClientActiveRequests,
                                                   unitMetricHttpClientActiveRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHttpClientActiveRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHttpClientActiveRequests,
                                                    descrMetricHttpClientActiveRequests,
                                                    unitMetricHttpClientActiveRequests);
}

/**
  The duration of the successfully established outbound HTTP connections.
  <p>
  histogram
 */
static constexpr const char *kMetricHttpClientConnectionDuration =
    "http.client.connection.duration";
static constexpr const char *descrMetricHttpClientConnectionDuration =
    "The duration of the successfully established outbound HTTP connections.";
static constexpr const char *unitMetricHttpClientConnectionDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpClientConnectionDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpClientConnectionDuration,
                                      descrMetricHttpClientConnectionDuration,
                                      unitMetricHttpClientConnectionDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpClientConnectionDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpClientConnectionDuration,
                                      descrMetricHttpClientConnectionDuration,
                                      unitMetricHttpClientConnectionDuration);
}

/**
  Number of outbound HTTP connections that are currently active or idle on the client.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHttpClientOpenConnections = "http.client.open_connections";
static constexpr const char *descrMetricHttpClientOpenConnections =
    "Number of outbound HTTP connections that are currently active or idle on the client.";
static constexpr const char *unitMetricHttpClientOpenConnections = "{connection}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHttpClientOpenConnections(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHttpClientOpenConnections,
                                         descrMetricHttpClientOpenConnections,
                                         unitMetricHttpClientOpenConnections);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHttpClientOpenConnections(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHttpClientOpenConnections,
                                          descrMetricHttpClientOpenConnections,
                                          unitMetricHttpClientOpenConnections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHttpClientOpenConnections(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHttpClientOpenConnections,
                                                   descrMetricHttpClientOpenConnections,
                                                   unitMetricHttpClientOpenConnections);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHttpClientOpenConnections(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHttpClientOpenConnections,
                                                    descrMetricHttpClientOpenConnections,
                                                    unitMetricHttpClientOpenConnections);
}

/**
  Size of HTTP client request bodies.
  <p>
  The size of the request payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size. <p> histogram
 */
static constexpr const char *kMetricHttpClientRequestBodySize = "http.client.request.body.size";
static constexpr const char *descrMetricHttpClientRequestBodySize =
    "Size of HTTP client request bodies.";
static constexpr const char *unitMetricHttpClientRequestBodySize = "By";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpClientRequestBodySize(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpClientRequestBodySize,
                                      descrMetricHttpClientRequestBodySize,
                                      unitMetricHttpClientRequestBodySize);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpClientRequestBodySize(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpClientRequestBodySize,
                                      descrMetricHttpClientRequestBodySize,
                                      unitMetricHttpClientRequestBodySize);
}

/**
  Duration of HTTP client requests.
  <p>
  histogram
 */
static constexpr const char *kMetricHttpClientRequestDuration = "http.client.request.duration";
static constexpr const char *descrMetricHttpClientRequestDuration =
    "Duration of HTTP client requests.";
static constexpr const char *unitMetricHttpClientRequestDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpClientRequestDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpClientRequestDuration,
                                      descrMetricHttpClientRequestDuration,
                                      unitMetricHttpClientRequestDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpClientRequestDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpClientRequestDuration,
                                      descrMetricHttpClientRequestDuration,
                                      unitMetricHttpClientRequestDuration);
}

/**
  Size of HTTP client response bodies.
  <p>
  The size of the response payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size. <p> histogram
 */
static constexpr const char *kMetricHttpClientResponseBodySize = "http.client.response.body.size";
static constexpr const char *descrMetricHttpClientResponseBodySize =
    "Size of HTTP client response bodies.";
static constexpr const char *unitMetricHttpClientResponseBodySize = "By";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpClientResponseBodySize(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpClientResponseBodySize,
                                      descrMetricHttpClientResponseBodySize,
                                      unitMetricHttpClientResponseBodySize);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpClientResponseBodySize(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpClientResponseBodySize,
                                      descrMetricHttpClientResponseBodySize,
                                      unitMetricHttpClientResponseBodySize);
}

/**
  Number of active HTTP server requests.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHttpServerActiveRequests = "http.server.active_requests";
static constexpr const char *descrMetricHttpServerActiveRequests =
    "Number of active HTTP server requests.";
static constexpr const char *unitMetricHttpServerActiveRequests = "{request}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHttpServerActiveRequests(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHttpServerActiveRequests,
                                         descrMetricHttpServerActiveRequests,
                                         unitMetricHttpServerActiveRequests);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHttpServerActiveRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHttpServerActiveRequests,
                                          descrMetricHttpServerActiveRequests,
                                          unitMetricHttpServerActiveRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHttpServerActiveRequests(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHttpServerActiveRequests,
                                                   descrMetricHttpServerActiveRequests,
                                                   unitMetricHttpServerActiveRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHttpServerActiveRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHttpServerActiveRequests,
                                                    descrMetricHttpServerActiveRequests,
                                                    unitMetricHttpServerActiveRequests);
}

/**
  Size of HTTP server request bodies.
  <p>
  The size of the request payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size. <p> histogram
 */
static constexpr const char *kMetricHttpServerRequestBodySize = "http.server.request.body.size";
static constexpr const char *descrMetricHttpServerRequestBodySize =
    "Size of HTTP server request bodies.";
static constexpr const char *unitMetricHttpServerRequestBodySize = "By";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpServerRequestBodySize(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpServerRequestBodySize,
                                      descrMetricHttpServerRequestBodySize,
                                      unitMetricHttpServerRequestBodySize);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpServerRequestBodySize(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpServerRequestBodySize,
                                      descrMetricHttpServerRequestBodySize,
                                      unitMetricHttpServerRequestBodySize);
}

/**
  Duration of HTTP server requests.
  <p>
  histogram
 */
static constexpr const char *kMetricHttpServerRequestDuration = "http.server.request.duration";
static constexpr const char *descrMetricHttpServerRequestDuration =
    "Duration of HTTP server requests.";
static constexpr const char *unitMetricHttpServerRequestDuration = "s";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpServerRequestDuration(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpServerRequestDuration,
                                      descrMetricHttpServerRequestDuration,
                                      unitMetricHttpServerRequestDuration);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpServerRequestDuration(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpServerRequestDuration,
                                      descrMetricHttpServerRequestDuration,
                                      unitMetricHttpServerRequestDuration);
}

/**
  Size of HTTP server response bodies.
  <p>
  The size of the response payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size. <p> histogram
 */
static constexpr const char *kMetricHttpServerResponseBodySize = "http.server.response.body.size";
static constexpr const char *descrMetricHttpServerResponseBodySize =
    "Size of HTTP server response bodies.";
static constexpr const char *unitMetricHttpServerResponseBodySize = "By";

static inline nostd::unique_ptr<metrics::Histogram<uint64_t>>
CreateSyncInt64MetricHttpServerResponseBodySize(metrics::Meter *meter)
{
  return meter->CreateUInt64Histogram(kMetricHttpServerResponseBodySize,
                                      descrMetricHttpServerResponseBodySize,
                                      unitMetricHttpServerResponseBodySize);
}

static inline nostd::unique_ptr<metrics::Histogram<double>>
CreateSyncDoubleMetricHttpServerResponseBodySize(metrics::Meter *meter)
{
  return meter->CreateDoubleHistogram(kMetricHttpServerResponseBodySize,
                                      descrMetricHttpServerResponseBodySize,
                                      unitMetricHttpServerResponseBodySize);
}

}  // namespace http
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
