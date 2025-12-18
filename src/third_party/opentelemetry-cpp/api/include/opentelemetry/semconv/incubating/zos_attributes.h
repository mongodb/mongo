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
namespace zos
{

/**
  The System Management Facility (SMF) Identifier uniquely identified a z/OS system within a SYSPLEX
  or mainframe environment and is used for system and performance analysis.
 */
static constexpr const char *kZosSmfId = "zos.smf.id";

/**
  The name of the SYSPLEX to which the z/OS system belongs too.
 */
static constexpr const char *kZosSysplexName = "zos.sysplex.name";

}  // namespace zos
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
