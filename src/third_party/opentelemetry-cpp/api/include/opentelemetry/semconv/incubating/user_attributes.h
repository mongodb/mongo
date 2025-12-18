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
namespace user
{

/**
  User email address.
 */
static constexpr const char *kUserEmail = "user.email";

/**
  User's full name
 */
static constexpr const char *kUserFullName = "user.full_name";

/**
  Unique user hash to correlate information for a user in anonymized form.
  <p>
  Useful if @code user.id @endcode or @code user.name @endcode contain confidential information and
  cannot be used.
 */
static constexpr const char *kUserHash = "user.hash";

/**
  Unique identifier of the user.
 */
static constexpr const char *kUserId = "user.id";

/**
  Short name or login/username of the user.
 */
static constexpr const char *kUserName = "user.name";

/**
  Array of user roles at the time of the event.
 */
static constexpr const char *kUserRoles = "user.roles";

}  // namespace user
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
