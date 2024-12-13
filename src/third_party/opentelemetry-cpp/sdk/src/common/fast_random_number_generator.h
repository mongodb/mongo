// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 * Profiling shows that random number generation can be a significant cost of
 * span generation. This provides a faster random number generator than
 * std::mt19937_64; and since we don't care about the other beneficial random
 * number properties that std:mt19937_64 provides for this application, it's a
 * entirely appropriate replacement.
 *
 * Note for Windows users - please make sure that NOMINMAX is defined, e.g.
 *
 * ...
 * #define NOMINMAX
 * #include <Windows.h>
 * ...
 *
 * See:
 * https://stackoverflow.com/questions/13416418/define-nominmax-using-stdmin-max
 *
 */
class FastRandomNumberGenerator
{
public:
  using result_type = uint64_t;

  FastRandomNumberGenerator() noexcept = default;

  template <class SeedSequence>
  FastRandomNumberGenerator(SeedSequence &seed_sequence) noexcept
  {
    seed(seed_sequence);
  }

  uint64_t operator()() noexcept
  {
    // Uses the xorshift128p random number generation algorithm described in
    // https://en.wikipedia.org/wiki/Xorshift
    auto &state_a = state_[0];
    auto &state_b = state_[1];
    auto t        = state_a;
    auto s        = state_b;
    state_a       = s;
    t ^= t << 23;        // a
    t ^= t >> 17;        // b
    t ^= s ^ (s >> 26);  // c
    state_b = t;
    return t + s;
  }

  // RandomNumberGenerator concept functions required from standard library.
  // See http://www.cplusplus.com/reference/random/mt19937/
  template <class SeedSequence>
  void seed(SeedSequence &seed_sequence) noexcept
  {
    seed_sequence.generate(reinterpret_cast<uint32_t *>(state_.data()),
                           reinterpret_cast<uint32_t *>(state_.data() + state_.size()));
  }

  static constexpr uint64_t(min)() noexcept { return 0; }

  static constexpr uint64_t(max)() noexcept { return (std::numeric_limits<uint64_t>::max)(); }

private:
  std::array<uint64_t, 2> state_{};
};
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
