// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/version.h"
#include "src/common/fast_random_number_generator.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 * Utility methods for creating random data, based on a seeded thread-local
 * number generator.
 */
class Random
{
public:
  /**
   * @return an unsigned 64 bit random number
   */
  static uint64_t GenerateRandom64() noexcept;
  /**
   * Fill the passed span with random bytes.
   *
   * @param buffer A span of bytes.
   */
  static void GenerateRandomBuffer(opentelemetry::nostd::span<uint8_t> buffer) noexcept;

private:
  /**
   * @return a seeded thread-local random number generator.
   */
  static FastRandomNumberGenerator &GetRandomNumberGenerator() noexcept;
};
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
