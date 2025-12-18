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
namespace webengine
{

/**
  Additional description of the web engine (e.g. detailed version and edition information).
 */
static constexpr const char *kWebengineDescription = "webengine.description";

/**
  The name of the web engine.
 */
static constexpr const char *kWebengineName = "webengine.name";

/**
  The version of the web engine.
 */
static constexpr const char *kWebengineVersion = "webengine.version";

}  // namespace webengine
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
