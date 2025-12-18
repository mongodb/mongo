/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace otel
{

/**
  A name uniquely identifying the instance of the OpenTelemetry component within its containing SDK
  instance. <p> Implementations SHOULD ensure a low cardinality for this attribute, even across
  application or SDK restarts. E.g. implementations MUST NOT use UUIDs as values for this attribute.
  <p>
  Implementations MAY achieve these goals by following a @code
  <otel.component.type>/<instance-counter> @endcode pattern, e.g. @code batching_span_processor/0
  @endcode. Hereby @code otel.component.type @endcode refers to the corresponding attribute value of
  the component. <p> The value of @code instance-counter @endcode MAY be automatically assigned by
  the component and uniqueness within the enclosing SDK instance MUST be guaranteed. For example,
  @code <instance-counter> @endcode MAY be implemented by using a monotonically increasing counter
  (starting with @code 0 @endcode), which is incremented every time an instance of the given
  component type is started. <p> With this implementation, for example the first Batching Span
  Processor would have @code batching_span_processor/0 @endcode as @code otel.component.name
  @endcode, the second one @code batching_span_processor/1 @endcode and so on. These values will
  therefore be reused in the case of an application restart.
 */
static constexpr const char *kOtelComponentName = "otel.component.name";

/**
  A name identifying the type of the OpenTelemetry component.
  <p>
  If none of the standardized values apply, implementations SHOULD use the language-defined name of
  the type. E.g. for Java the fully qualified classname SHOULD be used in this case.
 */
static constexpr const char *kOtelComponentType = "otel.component.type";

/**
  Deprecated. Use the @code otel.scope.name @endcode attribute

  @deprecated
  {"note": "Replaced by @code otel.scope.name @endcode.", "reason": "renamed", "renamed_to":
  "otel.scope.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kOtelLibraryName = "otel.library.name";

/**
  Deprecated. Use the @code otel.scope.version @endcode attribute.

  @deprecated
  {"note": "Replaced by @code otel.scope.version @endcode.", "reason": "renamed", "renamed_to":
  "otel.scope.version"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kOtelLibraryVersion = "otel.library.version";

/**
  The name of the instrumentation scope - (@code InstrumentationScope.Name @endcode in OTLP).
 */
static constexpr const char *kOtelScopeName = "otel.scope.name";

/**
  The schema URL of the instrumentation scope.
 */
static constexpr const char *kOtelScopeSchemaUrl = "otel.scope.schema_url";

/**
  The version of the instrumentation scope - (@code InstrumentationScope.Version @endcode in OTLP).
 */
static constexpr const char *kOtelScopeVersion = "otel.scope.version";

/**
  Determines whether the span has a parent span, and if so, <a
  href="https://opentelemetry.io/docs/specs/otel/trace/api/#isremote">whether it is a remote
  parent</a>
 */
static constexpr const char *kOtelSpanParentOrigin = "otel.span.parent.origin";

/**
  The result value of the sampler for this span
 */
static constexpr const char *kOtelSpanSamplingResult = "otel.span.sampling_result";

/**
  Name of the code, either "OK" or "ERROR". MUST NOT be set if the status code is UNSET.
 */
static constexpr const char *kOtelStatusCode = "otel.status_code";

/**
  Description of the Status if it has a value, otherwise not set.
 */
static constexpr const char *kOtelStatusDescription = "otel.status_description";

namespace OtelComponentTypeValues
{
/**
  The builtin SDK batching span processor
 */
static constexpr const char *kBatchingSpanProcessor = "batching_span_processor";

/**
  The builtin SDK simple span processor
 */
static constexpr const char *kSimpleSpanProcessor = "simple_span_processor";

/**
  The builtin SDK batching log record processor
 */
static constexpr const char *kBatchingLogProcessor = "batching_log_processor";

/**
  The builtin SDK simple log record processor
 */
static constexpr const char *kSimpleLogProcessor = "simple_log_processor";

/**
  OTLP span exporter over gRPC with protobuf serialization
 */
static constexpr const char *kOtlpGrpcSpanExporter = "otlp_grpc_span_exporter";

/**
  OTLP span exporter over HTTP with protobuf serialization
 */
static constexpr const char *kOtlpHttpSpanExporter = "otlp_http_span_exporter";

/**
  OTLP span exporter over HTTP with JSON serialization
 */
static constexpr const char *kOtlpHttpJsonSpanExporter = "otlp_http_json_span_exporter";

/**
  Zipkin span exporter over HTTP
 */
static constexpr const char *kZipkinHttpSpanExporter = "zipkin_http_span_exporter";

/**
  OTLP log record exporter over gRPC with protobuf serialization
 */
static constexpr const char *kOtlpGrpcLogExporter = "otlp_grpc_log_exporter";

/**
  OTLP log record exporter over HTTP with protobuf serialization
 */
static constexpr const char *kOtlpHttpLogExporter = "otlp_http_log_exporter";

/**
  OTLP log record exporter over HTTP with JSON serialization
 */
static constexpr const char *kOtlpHttpJsonLogExporter = "otlp_http_json_log_exporter";

/**
  The builtin SDK periodically exporting metric reader
 */
static constexpr const char *kPeriodicMetricReader = "periodic_metric_reader";

/**
  OTLP metric exporter over gRPC with protobuf serialization
 */
static constexpr const char *kOtlpGrpcMetricExporter = "otlp_grpc_metric_exporter";

/**
  OTLP metric exporter over HTTP with protobuf serialization
 */
static constexpr const char *kOtlpHttpMetricExporter = "otlp_http_metric_exporter";

/**
  OTLP metric exporter over HTTP with JSON serialization
 */
static constexpr const char *kOtlpHttpJsonMetricExporter = "otlp_http_json_metric_exporter";

/**
  Prometheus metric exporter over HTTP with the default text-based format
 */
static constexpr const char *kPrometheusHttpTextMetricExporter =
    "prometheus_http_text_metric_exporter";

}  // namespace OtelComponentTypeValues

namespace OtelSpanParentOriginValues
{
/**
  The span does not have a parent, it is a root span
 */
static constexpr const char *kNone = "none";

/**
  The span has a parent and the parent's span context <a
  href="https://opentelemetry.io/docs/specs/otel/trace/api/#isremote">isRemote()</a> is false
 */
static constexpr const char *kLocal = "local";

/**
  The span has a parent and the parent's span context <a
  href="https://opentelemetry.io/docs/specs/otel/trace/api/#isremote">isRemote()</a> is true
 */
static constexpr const char *kRemote = "remote";

}  // namespace OtelSpanParentOriginValues

namespace OtelSpanSamplingResultValues
{
/**
  The span is not sampled and not recording
 */
static constexpr const char *kDrop = "DROP";

/**
  The span is not sampled, but recording
 */
static constexpr const char *kRecordOnly = "RECORD_ONLY";

/**
  The span is sampled and recording
 */
static constexpr const char *kRecordAndSample = "RECORD_AND_SAMPLE";

}  // namespace OtelSpanSamplingResultValues

namespace OtelStatusCodeValues
{
/**
  The operation has been validated by an Application developer or Operator to have completed
  successfully.
 */
static constexpr const char *kOk = "OK";

/**
  The operation contains an error.
 */
static constexpr const char *kError = "ERROR";

}  // namespace OtelStatusCodeValues

}  // namespace otel
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
