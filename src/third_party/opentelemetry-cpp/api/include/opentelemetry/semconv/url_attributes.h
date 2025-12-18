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
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.3">URI path</a> component
  <p>
  Sensitive content provided in @code url.path @endcode SHOULD be scrubbed when instrumentations can
  identify it.
 */
static constexpr const char *kUrlPath = "url.path";

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
  The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.1">URI scheme</a> component
  identifying the used protocol.
 */
static constexpr const char *kUrlScheme = "url.scheme";

}  // namespace url
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
