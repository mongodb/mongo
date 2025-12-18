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
  <a href="https://tools.ietf.org/html/rfc7231#section-6">HTTP response status code</a>.
 */
static constexpr const char *kHttpResponseStatusCode = "http.response.status_code";

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
  Any HTTP method that the instrumentation has no prior knowledge of.
 */
static constexpr const char *kOther = "_OTHER";

}  // namespace HttpRequestMethodValues

}  // namespace http
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
