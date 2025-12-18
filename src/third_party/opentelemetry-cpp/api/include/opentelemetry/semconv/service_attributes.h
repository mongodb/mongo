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
namespace service
{

/**
  Logical name of the service.
  <p>
  MUST be the same for all instances of horizontally scaled services. If the value was not
  specified, SDKs MUST fallback to @code unknown_service: @endcode concatenated with <a
  href="process.md">@code process.executable.name @endcode</a>, e.g. @code unknown_service:bash
  @endcode. If @code process.executable.name @endcode is not available, the value MUST be set to
  @code unknown_service @endcode.
 */
static constexpr const char *kServiceName = "service.name";

/**
  The version string of the service API or implementation. The format is not defined by these
  conventions.
 */
static constexpr const char *kServiceVersion = "service.version";

}  // namespace service
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
