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
namespace dns
{

/**
  The list of IPv4 or IPv6 addresses resolved during DNS lookup.
 */
static constexpr const char *kDnsAnswers = "dns.answers";

/**
  The name being queried.
  <p>
  The name represents the queried domain name as it appears in the DNS query without any additional
  normalization.
 */
static constexpr const char *kDnsQuestionName = "dns.question.name";

}  // namespace dns
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
