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
namespace server
{

/**
  Server domain name if available without reverse DNS lookup; otherwise, IP address or Unix domain
  socket name. <p> When observed from the client side, and when communicating through an
  intermediary, @code server.address @endcode SHOULD represent the server address behind any
  intermediaries, for example proxies, if it's available.
 */
static constexpr const char *kServerAddress = "server.address";

/**
  Server port number.
  <p>
  When observed from the client side, and when communicating through an intermediary, @code
  server.port @endcode SHOULD represent the server port behind any intermediaries, for example
  proxies, if it's available.
 */
static constexpr const char *kServerPort = "server.port";

}  // namespace server
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
