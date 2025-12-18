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
namespace security_rule
{

/**
  A categorization value keyword used by the entity using the rule for detection of this event
 */
static constexpr const char *kSecurityRuleCategory = "security_rule.category";

/**
  The description of the rule generating the event.
 */
static constexpr const char *kSecurityRuleDescription = "security_rule.description";

/**
  Name of the license under which the rule used to generate this event is made available.
 */
static constexpr const char *kSecurityRuleLicense = "security_rule.license";

/**
  The name of the rule or signature generating the event.
 */
static constexpr const char *kSecurityRuleName = "security_rule.name";

/**
  Reference URL to additional information about the rule used to generate this event.
  <p>
  The URL can point to the vendor’s documentation about the rule. If that’s not available, it can
  also be a link to a more general page describing this type of alert.
 */
static constexpr const char *kSecurityRuleReference = "security_rule.reference";

/**
  Name of the ruleset, policy, group, or parent category in which the rule used to generate this
  event is a member.
 */
static constexpr const char *kSecurityRuleRulesetName = "security_rule.ruleset.name";

/**
  A rule ID that is unique within the scope of a set or group of agents, observers, or other
  entities using the rule for detection of this event.
 */
static constexpr const char *kSecurityRuleUuid = "security_rule.uuid";

/**
  The version / revision of the rule being used for analysis.
 */
static constexpr const char *kSecurityRuleVersion = "security_rule.version";

}  // namespace security_rule
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
