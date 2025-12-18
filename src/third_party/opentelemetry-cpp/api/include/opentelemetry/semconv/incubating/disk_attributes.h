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
namespace disk
{

/**
  The disk IO operation direction.
 */
static constexpr const char *kDiskIoDirection = "disk.io.direction";

namespace DiskIoDirectionValues
{

static constexpr const char *kRead = "read";

static constexpr const char *kWrite = "write";

}  // namespace DiskIoDirectionValues

}  // namespace disk
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
