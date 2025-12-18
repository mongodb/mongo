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
namespace browser
{

/**
  Array of brand name and version separated by a space
  <p>
  This value is intended to be taken from the <a
  href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> (@code
  navigator.userAgentData.brands @endcode).
 */
static constexpr const char *kBrowserBrands = "browser.brands";

/**
  Preferred language of the user using the browser
  <p>
  This value is intended to be taken from the Navigator API @code navigator.language @endcode.
 */
static constexpr const char *kBrowserLanguage = "browser.language";

/**
  A boolean that is true if the browser is running on a mobile device
  <p>
  This value is intended to be taken from the <a
  href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> (@code
  navigator.userAgentData.mobile @endcode). If unavailable, this attribute SHOULD be left unset.
 */
static constexpr const char *kBrowserMobile = "browser.mobile";

/**
  The platform on which the browser is running
  <p>
  This value is intended to be taken from the <a
  href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> (@code
  navigator.userAgentData.platform @endcode). If unavailable, the legacy @code navigator.platform
  @endcode API SHOULD NOT be used instead and this attribute SHOULD be left unset in order for the
  values to be consistent. The list of possible values is defined in the <a
  href="https://wicg.github.io/ua-client-hints/#sec-ch-ua-platform">W3C User-Agent Client Hints
  specification</a>. Note that some (but not all) of these values can overlap with values in the <a
  href="./os.md">@code os.type @endcode and @code os.name @endcode attributes</a>. However, for
  consistency, the values in the @code browser.platform @endcode attribute should capture the exact
  value that the user agent provides.
 */
static constexpr const char *kBrowserPlatform = "browser.platform";

}  // namespace browser
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
