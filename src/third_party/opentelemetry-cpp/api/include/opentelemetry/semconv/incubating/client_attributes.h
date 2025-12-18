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
namespace client
{

/**
  Client address - domain name if available without reverse DNS lookup; otherwise, IP address or
  Unix domain socket name. <p> When observed from the server side, and when communicating through an
  intermediary, @code client.address @endcode SHOULD represent the client address behind any
  intermediaries,  for example proxies, if it's available.
 */
static constexpr const char *kClientAddress = "client.address";

/**
  Client port number.
  <p>
  When observed from the server side, and when communicating through an intermediary, @code
  client.port @endcode SHOULD represent the client port behind any intermediaries,  for example
  proxies, if it's available.
 */
static constexpr const char *kClientPort = "client.port";

}  // namespace client
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
