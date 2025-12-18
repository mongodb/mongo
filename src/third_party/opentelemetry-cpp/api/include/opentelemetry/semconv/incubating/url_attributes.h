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
namespace url
{

/**
  Domain extracted from the @code url.full @endcode, such as "opentelemetry.io".
  <p>
  In some cases a URL may refer to an IP and/or port directly, without a domain name. In this case,
  the IP address would go to the domain field. If the URL contains a <a
  href="https://www.rfc-editor.org/rfc/rfc2732#section-2">literal IPv6 address</a> enclosed by @code
  [ @endcode and @code ] @endcode, the @code [ @endcode and @code ] @endcode characters should also
  be captured in the domain field.
 */
static constexpr const char *kUrlDomain = "url.domain";

/**
  The file extension extracted from the @code url.full @endcode, excluding the leading dot.
  <p>
  The file extension is only set if it exists, as not every url has a file extension. When the file
  name has multiple extensions @code example.tar.gz @endcode, only the last one should be captured
  @code gz @endcode, not @code tar.gz @endcode.
 */
static constexpr const char *kUrlExtension = "url.extension";

/**
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.5">URI fragment</a> component
 */
static constexpr const char *kUrlFragment = "url.fragment";

/**
  Absolute URL describing a network resource according to <a
  href="https://www.rfc-editor.org/rfc/rfc3986">RFC3986</a> <p> For network calls, URL usually has
  @code scheme://host[:port][path][?query][#fragment] @endcode format, where the fragment is not
  transmitted over HTTP, but if it is known, it SHOULD be included nevertheless. <p>
  @code url.full @endcode MUST NOT contain credentials passed via URL in form of @code
  https://username:password@www.example.com/ @endcode. In such case username and password SHOULD be
  redacted and attribute's value SHOULD be @code https://REDACTED:REDACTED@www.example.com/
  @endcode. <p>
  @code url.full @endcode SHOULD capture the absolute URL when it is available (or can be
  reconstructed). <p> Sensitive content provided in @code url.full @endcode SHOULD be scrubbed when
  instrumentations can identify it. <p>

  Query string values for the following keys SHOULD be redacted by default and replaced by the
  value @code REDACTED @endcode:
  <ul>
    <li><a
  href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/RESTAuthentication.html#RESTAuthenticationQueryStringAuth">@code
  AWSAccessKeyId @endcode</a></li> <li><a
  href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/RESTAuthentication.html#RESTAuthenticationQueryStringAuth">@code
  Signature @endcode</a></li> <li><a
  href="https://learn.microsoft.com/azure/storage/common/storage-sas-overview#sas-token">@code sig
  @endcode</a></li> <li><a
  href="https://cloud.google.com/storage/docs/access-control/signed-urls">@code X-Goog-Signature
  @endcode</a></li>
  </ul>
  <p>
  This list is subject to change over time.
  <p>
  When a query string value is redacted, the query string key SHOULD still be preserved, e.g.
  @code https://www.example.com/path?color=blue&sig=REDACTED @endcode.
 */
static constexpr const char *kUrlFull = "url.full";

/**
  Unmodified original URL as seen in the event source.
  <p>
  In network monitoring, the observed URL may be a full URL, whereas in access logs, the URL is
  often just represented as a path. This field is meant to represent the URL as it was observed,
  complete or not.
  @code url.original @endcode might contain credentials passed via URL in form of @code
  https://username:password@www.example.com/ @endcode. In such case password and username SHOULD NOT
  be redacted and attribute's value SHOULD remain the same.
 */
static constexpr const char *kUrlOriginal = "url.original";

/**
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.3">URI path</a> component
  <p>
  Sensitive content provided in @code url.path @endcode SHOULD be scrubbed when instrumentations can
  identify it.
 */
static constexpr const char *kUrlPath = "url.path";

/**
  Port extracted from the @code url.full @endcode
 */
static constexpr const char *kUrlPort = "url.port";

/**
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.4">URI query</a> component
  <p>
  Sensitive content provided in @code url.query @endcode SHOULD be scrubbed when instrumentations
  can identify it. <p>

  Query string values for the following keys SHOULD be redacted by default and replaced by the value
  @code REDACTED @endcode: <ul> <li><a
  href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/RESTAuthentication.html#RESTAuthenticationQueryStringAuth">@code
  AWSAccessKeyId @endcode</a></li> <li><a
  href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/RESTAuthentication.html#RESTAuthenticationQueryStringAuth">@code
  Signature @endcode</a></li> <li><a
  href="https://learn.microsoft.com/azure/storage/common/storage-sas-overview#sas-token">@code sig
  @endcode</a></li> <li><a
  href="https://cloud.google.com/storage/docs/access-control/signed-urls">@code X-Goog-Signature
  @endcode</a></li>
  </ul>
  <p>
  This list is subject to change over time.
  <p>
  When a query string value is redacted, the query string key SHOULD still be preserved, e.g.
  @code q=OpenTelemetry&sig=REDACTED @endcode.
 */
static constexpr const char *kUrlQuery = "url.query";

/**
  The highest registered url domain, stripped of the subdomain.
  <p>
  This value can be determined precisely with the <a href="https://publicsuffix.org/">public suffix
  list</a>. For example, the registered domain for @code foo.example.com @endcode is @code
  example.com @endcode. Trying to approximate this by simply taking the last two labels will not
  work well for TLDs such as @code co.uk @endcode.
 */
static constexpr const char *kUrlRegisteredDomain = "url.registered_domain";

/**
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.1">URI scheme</a> component
  identifying the used protocol.
 */
static constexpr const char *kUrlScheme = "url.scheme";

/**
  The subdomain portion of a fully qualified domain name includes all of the names except the host
  name under the registered_domain. In a partially qualified domain, or if the qualification level
  of the full name cannot be determined, subdomain contains all of the names below the registered
  domain. <p> The subdomain portion of @code www.east.mydomain.co.uk @endcode is @code east
  @endcode. If the domain has multiple levels of subdomain, such as @code sub2.sub1.example.com
  @endcode, the subdomain field should contain @code sub2.sub1 @endcode, with no trailing period.
 */
static constexpr const char *kUrlSubdomain = "url.subdomain";

/**
  The low-cardinality template of an <a
  href="https://www.rfc-editor.org/rfc/rfc3986#section-4.2">absolute path reference</a>.
 */
static constexpr const char *kUrlTemplate = "url.template";

/**
  The effective top level domain (eTLD), also known as the domain suffix, is the last part of the
  domain name. For example, the top level domain for example.com is @code com @endcode. <p> This
  value can be determined precisely with the <a href="https://publicsuffix.org/">public suffix
  list</a>.
 */
static constexpr const char *kUrlTopLevelDomain = "url.top_level_domain";

}  // namespace url
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
