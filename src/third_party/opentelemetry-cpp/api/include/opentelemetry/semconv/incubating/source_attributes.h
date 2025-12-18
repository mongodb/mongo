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
namespace source
{

/**
  Source address - domain name if available without reverse DNS lookup; otherwise, IP address or
  Unix domain socket name. <p> When observed from the destination side, and when communicating
  through an intermediary, @code source.address @endcode SHOULD represent the source address behind
  any intermediaries, for example proxies, if it's available.
 */
static constexpr const char *kSourceAddress = "source.address";

/**
  Source port number
 */
static constexpr const char *kSourcePort = "source.port";

}  // namespace source
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
