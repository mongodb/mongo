// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstring>
#include <string>

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 *
 * FNV Hashing Utilities
 *
 * Implements FNV-1a hashing algorithm for 32-bit and 64-bit types.
 *
 * - FNV (Fowler–Noll–Vo) is a simple, fast, and widely used non-cryptographic hash.
 * - FNV-1a is the recommended variant because it mixes input bits better.
 * - We parameterize by type size (4 = 32-bit, 8 = 64-bit).
 */

// Forward declaration for FNV prime constants
template <typename Ty, size_t Size>
struct FnvPrime;

// Specialization for 32-bit
template <typename Ty>
struct FnvPrime<Ty, 4>
{
  static constexpr Ty value = static_cast<Ty>(0x01000193U);
};

// Specialization for 64-bit
template <typename Ty>
struct FnvPrime<Ty, 8>
{
  static constexpr Ty value = static_cast<Ty>(0x100000001b3ULL);
};

// Forward declaration for FNV offset basis constants
template <typename Ty, size_t Size>
struct FnvOffset;

// Specialization for 32-bit
template <typename Ty>
struct FnvOffset<Ty, 4>
{
  // 32-bit offset basis
  static constexpr Ty value = static_cast<Ty>(0x811C9DC5U);

  static constexpr Ty Fix(Ty hval) noexcept { return hval; }
};

// Specialization for 64-bit
template <typename Ty>
struct FnvOffset<Ty, 8>
{
  // 64-bit offset basis
  static constexpr Ty value = static_cast<Ty>(0xCBF29CE484222325ULL);

  // Fix function: mix upper and lower bits for better distribution
  static constexpr Ty Fix(Ty hval) noexcept { return hval ^ (hval >> 32); }
};

/**
 * FNV-1a hash function
 *
 * @tparam Ty  Hash integer type (std::size_t, uint32_t, uint64_t, etc.)
 * @param buf  Pointer to the input buffer
 * @param len  Length of the buffer
 * @param hval Starting hash value (defaults to offset basis)
 * @return     Computed hash
 */
template <typename Ty>
inline Ty Fnv1a(const void *buf, size_t len, Ty hval = FnvOffset<Ty, sizeof(Ty)>::value) noexcept
{
  const unsigned char *bp = reinterpret_cast<const unsigned char *>(buf);
  const unsigned char *be = bp + len;
  Ty prime                = FnvPrime<Ty, sizeof(Ty)>::value;

  while (bp < be)
  {
    hval ^= static_cast<Ty>(*bp++);
    hval *= prime;
  }
  return FnvOffset<Ty, sizeof(Ty)>::Fix(hval);
}

/**
 * Hash and equality for nostd::string_view, enabling safe use in unordered_map
 * without requiring null termination.
 */

struct StringViewHash
{
#if defined(OPENTELEMETRY_STL_VERSION)
#  if OPENTELEMETRY_STL_VERSION >= 2020
  using is_transparent = void;
#  endif
#endif

  std::size_t operator()(const std::string &s) const noexcept
  {
    return Fnv1a<std::size_t>(s.data(), s.size());
  }

  std::size_t operator()(opentelemetry::nostd::string_view sv) const noexcept
  {
    return Fnv1a<std::size_t>(sv.data(), sv.size());
  }
};

struct StringViewEqual
{
#if defined(OPENTELEMETRY_STL_VERSION)
#  if OPENTELEMETRY_STL_VERSION >= 2020
  using is_transparent = void;
#  endif
#endif

  template <typename Lhs, typename Rhs>
  bool operator()(const Lhs &lhs, const Rhs &rhs) const noexcept
  {
    opentelemetry::nostd::string_view lsv(lhs);
    opentelemetry::nostd::string_view rsv(rhs);

    return lsv.size() == rsv.size() && std::memcmp(lsv.data(), rsv.data(), lsv.size()) == 0;
  }
};

/**
 * Cross-platform heterogeneous lookup wrapper.
 * Falls back to std::string construction on libc++ (macOS) and pre-c++20,
 * but uses direct lookup on libstdc++ (Linux).
 */

template <typename MapType>
inline auto find_heterogeneous(MapType &&map, opentelemetry::nostd::string_view key)
{
#if defined(_LIBCPP_VERSION) || \
    (!defined(OPENTELEMETRY_STL_VERSION) || OPENTELEMETRY_STL_VERSION < 2020)
  return map.find(std::string(key));
#else
  // libstdc++ + C++20: heterogeneous lookup works
  return map.find(key);
#endif
}
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
