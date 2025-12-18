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
namespace cpu
{

/**
  The logical CPU number [0..n-1]
 */
static constexpr const char *kCpuLogicalNumber = "cpu.logical_number";

/**
  The mode of the CPU
 */
static constexpr const char *kCpuMode = "cpu.mode";

namespace CpuModeValues
{
/**
  User
 */
static constexpr const char *kUser = "user";

/**
  System
 */
static constexpr const char *kSystem = "system";

/**
  Nice
 */
static constexpr const char *kNice = "nice";

/**
  Idle
 */
static constexpr const char *kIdle = "idle";

/**
  IO Wait
 */
static constexpr const char *kIowait = "iowait";

/**
  Interrupt
 */
static constexpr const char *kInterrupt = "interrupt";

/**
  Steal
 */
static constexpr const char *kSteal = "steal";

/**
  Kernel
 */
static constexpr const char *kKernel = "kernel";

}  // namespace CpuModeValues

}  // namespace cpu
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
