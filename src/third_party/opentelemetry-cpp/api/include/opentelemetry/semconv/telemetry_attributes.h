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
namespace telemetry
{

/**
  The language of the telemetry SDK.
 */
static constexpr const char *kTelemetrySdkLanguage = "telemetry.sdk.language";

/**
  The name of the telemetry SDK as defined above.
  <p>
  The OpenTelemetry SDK MUST set the @code telemetry.sdk.name @endcode attribute to @code
  opentelemetry @endcode. If another SDK, like a fork or a vendor-provided implementation, is used,
  this SDK MUST set the
  @code telemetry.sdk.name @endcode attribute to the fully-qualified class or module name of this
  SDK's main entry point or another suitable identifier depending on the language. The identifier
  @code opentelemetry @endcode is reserved and MUST NOT be used in this case. All custom identifiers
  SHOULD be stable across different versions of an implementation.
 */
static constexpr const char *kTelemetrySdkName = "telemetry.sdk.name";

/**
  The version string of the telemetry SDK.
 */
static constexpr const char *kTelemetrySdkVersion = "telemetry.sdk.version";

namespace TelemetrySdkLanguageValues
{

static constexpr const char *kCpp = "cpp";

static constexpr const char *kDotnet = "dotnet";

static constexpr const char *kErlang = "erlang";

static constexpr const char *kGo = "go";

static constexpr const char *kJava = "java";

static constexpr const char *kNodejs = "nodejs";

static constexpr const char *kPhp = "php";

static constexpr const char *kPython = "python";

static constexpr const char *kRuby = "ruby";

static constexpr const char *kRust = "rust";

static constexpr const char *kSwift = "swift";

static constexpr const char *kWebjs = "webjs";

}  // namespace TelemetrySdkLanguageValues

}  // namespace telemetry
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
