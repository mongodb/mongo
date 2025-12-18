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
namespace device
{

/**
  A unique identifier representing the device
  <p>
  Its value SHOULD be identical for all apps on a device and it SHOULD NOT change if an app is
  uninstalled and re-installed. However, it might be resettable by the user for all apps on a
  device. Hardware IDs (e.g. vendor-specific serial number, IMEI or MAC address) MAY be used as
  values. <p> More information about Android identifier best practices can be found in the <a
  href="https://developer.android.com/training/articles/user-data-ids">Android user data IDs
  guide</a>. <blockquote>
  [!WARNING]
  <p>
  This attribute may contain sensitive (PII) information. Caution should be taken when storing
  personal data or anything which can identify a user. GDPR and data protection laws may apply,
  ensure you do your own due diligence.
  <p>
  Due to these reasons, this identifier is not recommended for consumer applications and will likely
  result in rejection from both Google Play and App Store. However, it may be appropriate for
  specific enterprise scenarios, such as kiosk devices or enterprise-managed devices, with
  appropriate compliance clearance. Any instrumentation providing this identifier MUST implement it
  as an opt-in feature. <p> See <a href="/docs/registry/attributes/app.md#app-installation-id">@code
  app.installation.id @endcode</a> for a more privacy-preserving alternative.</blockquote>
 */
static constexpr const char *kDeviceId = "device.id";

/**
  The name of the device manufacturer
  <p>
  The Android OS provides this field via <a
  href="https://developer.android.com/reference/android/os/Build#MANUFACTURER">Build</a>. iOS apps
  SHOULD hardcode the value @code Apple @endcode.
 */
static constexpr const char *kDeviceManufacturer = "device.manufacturer";

/**
  The model identifier for the device
  <p>
  It's recommended this value represents a machine-readable version of the model identifier rather
  than the market or consumer-friendly name of the device.
 */
static constexpr const char *kDeviceModelIdentifier = "device.model.identifier";

/**
  The marketing name for the device model
  <p>
  It's recommended this value represents a human-readable version of the device model rather than a
  machine-readable alternative.
 */
static constexpr const char *kDeviceModelName = "device.model.name";

}  // namespace device
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
