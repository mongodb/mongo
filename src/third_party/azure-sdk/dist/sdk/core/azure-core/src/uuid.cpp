// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/uuid.hpp"

#include "azure/core/internal/strings.hpp"
#include "azure/core/platform.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <type_traits>

#if defined(AZ_PLATFORM_POSIX)
#include <thread>
namespace {
// 64-bit Mersenne Twister by Matsumoto and Nishimura, 2000
// Used to generate the random numbers for the Uuid.
// The seed is generated with std::random_device.
static thread_local std::mt19937_64 randomGenerator(std::random_device{}());
} // namespace
#endif

using Azure::Core::_internal::StringExtensions;

namespace {
/*
"00000000-0000-0000-0000-000000000000"
         ^    ^    ^    ^
 000000000011111111112222222222333333
 012345678901234567890123456789012345
 \______________ = 36 ______________/
*/
constexpr size_t UuidStringLength = 36;
constexpr bool IsDashIndex(size_t i) { return i == 8 || i == 13 || i == 18 || i == 23; }

constexpr std::uint8_t HexToNibble(char c) // does not check for errors
{
  if (c >= 'a')
  {
    return 10 + (c - 'a');
  }

  if (c >= 'A')
  {
    return 10 + (c - 'A');
  }

  return c - '0';
}

constexpr char NibbleToHex(std::uint8_t nibble) // does not check for errors
{
  if (nibble <= 9)
  {
    return '0' + nibble;
  }

  return 'a' + (nibble - 10);
}
} // namespace

namespace Azure { namespace Core {
  std::string Uuid::ToString() const
  {
    std::string s(UuidStringLength, '-');

    for (size_t bi = 0, si = 0; bi < m_uuid.size(); ++bi)
    {
      if (IsDashIndex(si))
      {
        ++si;
      }

      assert((si < UuidStringLength) && (si + 1 < UuidStringLength));

      const std::uint8_t b = m_uuid[bi];
      s[si] = NibbleToHex((b >> 4) & 0x0F);
      s[si + 1] = NibbleToHex(b & 0x0F);
      si += 2;
    }

    return s;
  }

  Uuid Uuid::Parse(std::string const& uuidString)
  {
    bool parseError = false;
    Uuid result;
    if (uuidString.size() != UuidStringLength)
    {
      parseError = true;
    }
    else
    {
      // si = string index, bi = byte index
      for (size_t si = 0, bi = 0; si < UuidStringLength; ++si)
      {
        const auto c = uuidString[si];
        if (IsDashIndex(si))
        {
          if (c != '-')
          {
            parseError = true;
            break;
          }
        }
        else
        {
          assert(si + 1 < UuidStringLength && bi < result.m_uuid.size());

          const auto c2 = uuidString[si + 1];
          if (!StringExtensions::IsHexDigit(c) || !StringExtensions::IsHexDigit(c2))
          {
            parseError = true;
            break;
          }

          result.m_uuid[bi] = (HexToNibble(c) << 4) | HexToNibble(c2);
          ++si;
          ++bi;
        }
      }
    }

    return parseError ? throw std::invalid_argument(
               "Error parsing Uuid: '" + uuidString
               + "' is not in the '00112233-4455-6677-8899-aAbBcCdDeEfF' format.")
                      : result;
  }

  Uuid Uuid::CreateUuid()
  {
    Uuid result{};

    // Using RngResultType and RngResultSize to highlight the places where the same type is used.
    using RngResultType = std::uint32_t;
    static_assert(sizeof(RngResultType) == 4, "sizeof(RngResultType) must be 4.");
    constexpr size_t RngResultSize = 4;

#if defined(AZ_PLATFORM_WINDOWS)
    std::random_device rd;

    static_assert(
        std::is_same<RngResultType, decltype(rd())>::value,
        "random_device::result_type must be of RngResultType.");
#else
    std::uniform_int_distribution<RngResultType> distribution;
#endif

    for (size_t i = 0; i < result.m_uuid.size(); i += RngResultSize)
    {
#if defined(AZ_PLATFORM_WINDOWS)
      const RngResultType x = rd();
#else
      const RngResultType x = distribution(randomGenerator);
#endif
      std::memcpy(result.m_uuid.data() + i, &x, RngResultSize);
    }

    // The variant field consists of a variable number of the most significant bits of octet 8 of
    // the UUID.
    // https://www.rfc-editor.org/rfc/rfc9562.html#name-variant-field
    // For setting the variant to conform to RFC9562, the high bits need to be of the form 10xx,
    // which means the hex value of the first 4 bits can only be either 8, 9, A|a, B|b. The 0-7
    // values are reserved for backward compatibility. The C|c, D|d values are reserved for
    // Microsoft, and the E|e, F|f values are reserved for future use.
    // Therefore, we have to zero out the two high bits, and then set the highest bit to 1.
    result.m_uuid.data()[8] = (result.m_uuid.data()[8] & 0x3F) | 0x80;

    {
      // https://www.rfc-editor.org/rfc/rfc9562.html#name-version-field
      constexpr std::uint8_t Version = 4; // Version 4: Pseudo-random number
      result.m_uuid.data()[6] = (result.m_uuid.data()[6] & 0xF) | (Version << 4);
    }

    return result;
  }
}} // namespace Azure::Core
