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
namespace linux
{

/**
  The Linux Slab memory state
 */
static constexpr const char *kLinuxMemorySlabState = "linux.memory.slab.state";

namespace LinuxMemorySlabStateValues
{

static constexpr const char *kReclaimable = "reclaimable";

static constexpr const char *kUnreclaimable = "unreclaimable";

}  // namespace LinuxMemorySlabStateValues

}  // namespace linux
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
