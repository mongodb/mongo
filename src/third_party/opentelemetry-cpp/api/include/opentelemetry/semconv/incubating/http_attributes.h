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
namespace http
{

/**
  Deprecated, use @code client.address @endcode instead.

  @deprecated
  {"note": "Replaced by @code client.address @endcode.", "reason": "renamed", "renamed_to":
  "client.address"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpClientIp = "http.client_ip";

/**
  State of the HTTP connection in the HTTP connection pool.
 */
static constexpr const char *kHttpConnectionState = "http.connection.state";

/**
  Deprecated, use @code network.protocol.name @endcode and @code network.protocol.version @endcode
  instead.

  @deprecated
  {"note": "Split into @code network.protocol.name @endcode and @code network.protocol.version
  @endcode", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpFlavor = "http.flavor";

/**
  Deprecated, use one of @code server.address @endcode, @code client.address @endcode or @code
  http.request.header.host @endcode instead, depending on the usage.

  @deprecated
  {"note": "Replaced by one of @code server.address @endcode, @code client.address @endcode or @code
  http.request.header.host @endcode, depending on the usage.\n", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpHost = "http.host";

/**
  Deprecated, use @code http.request.method @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.request.method @endcode.", "reason": "renamed", "renamed_to":
  "http.request.method"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpMethod = "http.method";

/**
  The size of the request payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size.
 */
static constexpr const char *kHttpRequestBodySize = "http.request.body.size";

/**
  HTTP request headers, @code <key> @endcode being the normalized HTTP Header name (lowercase), the
  value being the header values. <p> Instrumentations SHOULD require an explicit configuration of
  which headers are to be captured. Including all request headers can be a security risk - explicit
  configuration helps avoid leaking sensitive information. <p> The @code User-Agent @endcode header
  is already captured in the @code user_agent.original @endcode attribute. Users MAY explicitly
  configure instrumentations to capture them even though it is not recommended. <p> The attribute
  value MUST consist of either multiple header values as an array of strings or a single-item array
  containing a possibly comma-concatenated string, depending on the way the HTTP library provides
  access to headers. <p> Examples: <ul> <li>A header @code Content-Type: application/json @endcode
  SHOULD be recorded as the @code http.request.header.content-type @endcode attribute with value
  @code ["application/json"] @endcode.</li> <li>A header @code X-Forwarded-For: 1.2.3.4, 1.2.3.5
  @endcode SHOULD be recorded as the @code http.request.header.x-forwarded-for @endcode attribute
  with value @code ["1.2.3.4", "1.2.3.5"] @endcode or @code ["1.2.3.4, 1.2.3.5"] @endcode depending
  on the HTTP library.</li>
  </ul>
 */
static constexpr const char *kHttpRequestHeader = "http.request.header";

/**
  HTTP request method.
  <p>
  HTTP request method value SHOULD be "known" to the instrumentation.
  By default, this convention defines "known" methods as the ones listed in <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#name-methods">RFC9110</a>, the PATCH method
  defined in <a href="https://www.rfc-editor.org/rfc/rfc5789.html">RFC5789</a> and the QUERY method
  defined in <a
  href="https://datatracker.ietf.org/doc/draft-ietf-httpbis-safe-method-w-body/?include_text=1">httpbis-safe-method-w-body</a>.
  <p>
  If the HTTP request method is not known to instrumentation, it MUST set the @code
  http.request.method @endcode attribute to @code _OTHER @endcode. <p> If the HTTP instrumentation
  could end up converting valid HTTP request methods to @code _OTHER @endcode, then it MUST provide
  a way to override the list of known HTTP methods. If this override is done via environment
  variable, then the environment variable MUST be named OTEL_INSTRUMENTATION_HTTP_KNOWN_METHODS and
  support a comma-separated list of case-sensitive known HTTP methods (this list MUST be a full
  override of the default known method, it is not a list of known methods in addition to the
  defaults). <p> HTTP method names are case-sensitive and @code http.request.method @endcode
  attribute value MUST match a known HTTP method name exactly. Instrumentations for specific web
  frameworks that consider HTTP methods to be case insensitive, SHOULD populate a canonical
  equivalent. Tracing instrumentations that do so, MUST also set @code http.request.method_original
  @endcode to the original value.
 */
static constexpr const char *kHttpRequestMethod = "http.request.method";

/**
  Original HTTP method sent by the client in the request line.
 */
static constexpr const char *kHttpRequestMethodOriginal = "http.request.method_original";

/**
  The ordinal number of request resending attempt (for any reason, including redirects).
  <p>
  The resend count SHOULD be updated each time an HTTP request gets resent by the client, regardless
  of what was the cause of the resending (e.g. redirection, authorization failure, 503 Server
  Unavailable, network issues, or any other).
 */
static constexpr const char *kHttpRequestResendCount = "http.request.resend_count";

/**
  The total size of the request in bytes. This should be the total number of bytes sent over the
  wire, including the request line (HTTP/1.1), framing (HTTP/2 and HTTP/3), headers, and request
  body if any.
 */
static constexpr const char *kHttpRequestSize = "http.request.size";

/**
  Deprecated, use @code http.request.header.content-length @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.request.header.content-length @endcode.", "reason":
  "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpRequestContentLength =
    "http.request_content_length";

/**
  Deprecated, use @code http.request.body.size @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.request.body.size @endcode.", "reason": "renamed", "renamed_to":
  "http.request.body.size"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpRequestContentLengthUncompressed =
    "http.request_content_length_uncompressed";

/**
  The size of the response payload body in bytes. This is the number of bytes transferred excluding
  headers and is often, but not always, present as the <a
  href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a> header.
  For requests using transport encoding, this should be the compressed size.
 */
static constexpr const char *kHttpResponseBodySize = "http.response.body.size";

/**
  HTTP response headers, @code <key> @endcode being the normalized HTTP Header name (lowercase), the
  value being the header values. <p> Instrumentations SHOULD require an explicit configuration of
  which headers are to be captured. Including all response headers can be a security risk - explicit
  configuration helps avoid leaking sensitive information. <p> Users MAY explicitly configure
  instrumentations to capture them even though it is not recommended. <p> The attribute value MUST
  consist of either multiple header values as an array of strings or a single-item array containing
  a possibly comma-concatenated string, depending on the way the HTTP library provides access to
  headers. <p> Examples: <ul> <li>A header @code Content-Type: application/json @endcode header
  SHOULD be recorded as the @code http.request.response.content-type @endcode attribute with value
  @code ["application/json"] @endcode.</li> <li>A header @code My-custom-header: abc, def @endcode
  header SHOULD be recorded as the @code http.response.header.my-custom-header @endcode attribute
  with value @code ["abc", "def"] @endcode or @code ["abc, def"] @endcode depending on the HTTP
  library.</li>
  </ul>
 */
static constexpr const char *kHttpResponseHeader = "http.response.header";

/**
  The total size of the response in bytes. This should be the total number of bytes sent over the
  wire, including the status line (HTTP/1.1), framing (HTTP/2 and HTTP/3), headers, and response
  body and trailers if any.
 */
static constexpr const char *kHttpResponseSize = "http.response.size";

/**
  <a href="https://tools.ietf.org/html/rfc7231#section-6">HTTP response status code</a>.
 */
static constexpr const char *kHttpResponseStatusCode = "http.response.status_code";

/**
  Deprecated, use @code http.response.header.content-length @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.response.header.content-length @endcode.", "reason":
  "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpResponseContentLength =
    "http.response_content_length";

/**
  Deprecated, use @code http.response.body.size @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.response.body.size @endcode.", "reason": "renamed", "renamed_to":
  "http.response.body.size"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpResponseContentLengthUncompressed =
    "http.response_content_length_uncompressed";

/**
  The matched route template for the request. This MUST be low-cardinality and include all static
  path segments, with dynamic path segments represented with placeholders. <p> MUST NOT be populated
  when this is not supported by the HTTP server framework as the route attribute should have
  low-cardinality and the URI path can NOT substitute it. SHOULD include the <a
  href="/docs/http/http-spans.md#http-server-definitions">application root</a> if there is one. <p>
  A static path segment is a part of the route template with a fixed, low-cardinality value. This
  includes literal strings like @code /users/ @endcode and placeholders that are constrained to a
  finite, predefined set of values, e.g. @code {controller} @endcode or @code {action} @endcode. <p>
  A dynamic path segment is a placeholder for a value that can have high cardinality and is not
  constrained to a predefined list like static path segments. <p> Instrumentations SHOULD use
  routing information provided by the corresponding web framework. They SHOULD pick the most precise
  source of routing information and MAY support custom route formatting. Instrumentations SHOULD
  document the format and the API used to obtain the route string.
 */
static constexpr const char *kHttpRoute = "http.route";

/**
  Deprecated, use @code url.scheme @endcode instead.

  @deprecated
  {"note": "Replaced by @code url.scheme @endcode.", "reason": "renamed", "renamed_to":
  "url.scheme"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpScheme = "http.scheme";

/**
  Deprecated, use @code server.address @endcode instead.

  @deprecated
  {"note": "Replaced by @code server.address @endcode.", "reason": "renamed", "renamed_to":
  "server.address"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpServerName = "http.server_name";

/**
  Deprecated, use @code http.response.status_code @endcode instead.

  @deprecated
  {"note": "Replaced by @code http.response.status_code @endcode.", "reason": "renamed",
  "renamed_to": "http.response.status_code"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpStatusCode = "http.status_code";

/**
  Deprecated, use @code url.path @endcode and @code url.query @endcode instead.

  @deprecated
  {"note": "Split to @code url.path @endcode and @code url.query @endcode.", "reason": "obsoleted"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpTarget = "http.target";

/**
  Deprecated, use @code url.full @endcode instead.

  @deprecated
  {"note": "Replaced by @code url.full @endcode.", "reason": "renamed", "renamed_to": "url.full"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpUrl = "http.url";

/**
  Deprecated, use @code user_agent.original @endcode instead.

  @deprecated
  {"note": "Replaced by @code user_agent.original @endcode.", "reason": "renamed", "renamed_to":
  "user_agent.original"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kHttpUserAgent = "http.user_agent";

namespace HttpConnectionStateValues
{
/**
  active state.
 */
static constexpr const char *kActive = "active";

/**
  idle state.
 */
static constexpr const char *kIdle = "idle";

}  // namespace HttpConnectionStateValues

namespace HttpFlavorValues
{
/**
  HTTP/1.0
 */
static constexpr const char *kHttp10 = "1.0";

/**
  HTTP/1.1
 */
static constexpr const char *kHttp11 = "1.1";

/**
  HTTP/2
 */
static constexpr const char *kHttp20 = "2.0";

/**
  HTTP/3
 */
static constexpr const char *kHttp30 = "3.0";

/**
  SPDY protocol.
 */
static constexpr const char *kSpdy = "SPDY";

/**
  QUIC protocol.
 */
static constexpr const char *kQuic = "QUIC";

}  // namespace HttpFlavorValues

namespace HttpRequestMethodValues
{
/**
  CONNECT method.
 */
static constexpr const char *kConnect = "CONNECT";

/**
  DELETE method.
 */
static constexpr const char *kDelete = "DELETE";

/**
  GET method.
 */
static constexpr const char *kGet = "GET";

/**
  HEAD method.
 */
static constexpr const char *kHead = "HEAD";

/**
  OPTIONS method.
 */
static constexpr const char *kOptions = "OPTIONS";

/**
  PATCH method.
 */
static constexpr const char *kPatch = "PATCH";

/**
  POST method.
 */
static constexpr const char *kPost = "POST";

/**
  PUT method.
 */
static constexpr const char *kPut = "PUT";

/**
  TRACE method.
 */
static constexpr const char *kTrace = "TRACE";

/**
  QUERY method.
 */
static constexpr const char *kQuery = "QUERY";

/**
  Any HTTP method that the instrumentation has no prior knowledge of.
 */
static constexpr const char *kOther = "_OTHER";

}  // namespace HttpRequestMethodValues

}  // namespace http
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
