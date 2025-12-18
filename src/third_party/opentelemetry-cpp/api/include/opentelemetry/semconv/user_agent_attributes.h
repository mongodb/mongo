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
namespace user_agent
{

/**
  Value of the <a href="https://www.rfc-editor.org/rfc/rfc9110.html#field.user-agent">HTTP
  User-Agent</a> header sent by the client.
 */
static constexpr const char *kUserAgentOriginal = "user_agent.original";

}  // namespace user_agent
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
