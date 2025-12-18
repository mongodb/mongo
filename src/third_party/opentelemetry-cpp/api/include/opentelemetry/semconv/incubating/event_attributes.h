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
namespace event
{

/**
  Identifies the class / type of event.

  @deprecated
  {"note": "The value of this attribute MUST now be set as the value of the EventName field on the
  LogRecord to indicate that the LogRecord represents an Event.\n", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kEventName = "event.name";

}  // namespace event
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
