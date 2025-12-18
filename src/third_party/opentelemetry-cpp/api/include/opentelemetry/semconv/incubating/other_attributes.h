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
namespace other
{

/**
  Deprecated, use @code db.client.connection.state @endcode instead.

  @deprecated
  {"note": "Replaced by @code db.client.connection.state @endcode.", "reason": "renamed",
  "renamed_to": "db.client.connection.state"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kState = "state";

namespace StateValues
{

static constexpr const char *kIdle = "idle";

static constexpr const char *kUsed = "used";

}  // namespace StateValues

}  // namespace other
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
