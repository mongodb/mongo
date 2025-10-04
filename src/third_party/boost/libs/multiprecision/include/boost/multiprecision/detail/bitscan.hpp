///////////////////////////////////////////////////////////////
//  Copyright 2013 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
//
// Comparison operators for cpp_int_backend:
//
#ifndef BOOST_MP_DETAIL_BITSCAN_HPP
#define BOOST_MP_DETAIL_BITSCAN_HPP

#include <cstdint>
#include <climits>
#include <type_traits>
#include <boost/multiprecision/detail/endian.hpp>
#include <boost/multiprecision/detail/standalone_config.hpp>

#if (defined(BOOST_MSVC) || (defined(__clang__) && defined(__c2__)) || (defined(BOOST_INTEL) && defined(_MSC_VER))) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#endif

namespace boost { namespace multiprecision { namespace detail {

template <class Unsigned>
inline BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb_default(Unsigned mask)
{
   std::size_t result = 0;
   while (!(mask & 1u))
   {
      mask >>= 1;
      ++result;
   }
   return result;
}

template <class Unsigned>
inline BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb_default(Unsigned mask)
{
   std::size_t index = 0;
   while (mask)
   {
      ++index;
      mask >>= 1;
   }
   return --index;
}

template <class Unsigned>
inline BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb(Unsigned mask, const std::integral_constant<int, 0>&)
{
   return find_lsb_default(mask);
}

template <class Unsigned>
inline BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb(Unsigned mask, const std::integral_constant<int, 0>&)
{
   return find_msb_default(mask);
}

#if (defined(BOOST_MSVC) || (defined(__clang__) && defined(__c2__)) || (defined(BOOST_INTEL) && defined(_MSC_VER))) && (defined(_M_IX86) || defined(_M_X64))

#pragma intrinsic(_BitScanForward, _BitScanReverse)

BOOST_FORCEINLINE std::size_t find_lsb(unsigned long mask, const std::integral_constant<int, 1>&)
{
   unsigned long result;
   _BitScanForward(&result, mask);
   return result;
}

BOOST_FORCEINLINE std::size_t find_msb(unsigned long mask, const std::integral_constant<int, 1>&)
{
   unsigned long result;
   _BitScanReverse(&result, mask);
   return result;
}
#ifdef _M_X64

#pragma intrinsic(_BitScanForward64, _BitScanReverse64)

BOOST_FORCEINLINE std::size_t find_lsb(unsigned __int64 mask, const std::integral_constant<int, 2>&)
{
   unsigned long result;
   _BitScanForward64(&result, mask);
   return result;
}
template <class Unsigned>
BOOST_FORCEINLINE std::size_t find_msb(Unsigned mask, const std::integral_constant<int, 2>&)
{
   unsigned long result;
   _BitScanReverse64(&result, mask);
   return result;
}
#endif

template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned long),
       std::integral_constant<int, 1>,
#ifdef _M_X64
       typename std::conditional<
           sizeof(Unsigned) <= sizeof(__int64),
           std::integral_constant<int, 2>,
           std::integral_constant<int, 0> >::type
#else
       std::integral_constant<int, 0>
#endif
       >::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_lsb_default(mask);
   }
   else
#endif
      return find_lsb(static_cast<ui_type>(mask), tag_type());
}

template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned long),
       std::integral_constant<int, 1>,
#ifdef _M_X64
       typename std::conditional<
           sizeof(Unsigned) <= sizeof(__int64),
           std::integral_constant<int, 2>,
           std::integral_constant<int, 0> >::type
#else
       std::integral_constant<int, 0>
#endif
       >::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_msb_default(mask);
   }
   else
#endif
      return find_msb(static_cast<ui_type>(mask), tag_type());
}

#elif defined(BOOST_GCC) || defined(__clang__) || (defined(BOOST_INTEL) && defined(__GNUC__))

BOOST_FORCEINLINE std::size_t find_lsb(std::size_t mask, std::integral_constant<int, 1> const&)
{
   return static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned int>(mask)));
}
BOOST_FORCEINLINE std::size_t find_lsb(unsigned long mask, std::integral_constant<int, 2> const&)
{
   return static_cast<std::size_t>(__builtin_ctzl(static_cast<unsigned long>(mask)));
}
BOOST_FORCEINLINE std::size_t find_lsb(unsigned long long mask, std::integral_constant<int, 3> const&)
{
   return static_cast<std::size_t>(__builtin_ctzll(static_cast<unsigned long long>(mask)));
}
BOOST_FORCEINLINE std::size_t find_msb(std::size_t mask, std::integral_constant<int, 1> const&)
{
   return static_cast<std::size_t>(static_cast<std::size_t>(sizeof(unsigned) * static_cast<std::size_t>(CHAR_BIT) - 1u) - static_cast<std::size_t>(__builtin_clz(static_cast<unsigned int>(mask))));
}
BOOST_FORCEINLINE std::size_t find_msb(unsigned long mask, std::integral_constant<int, 2> const&)
{
   return static_cast<std::size_t>(static_cast<std::size_t>(sizeof(unsigned long) * static_cast<std::size_t>(CHAR_BIT) - 1u) - static_cast<std::size_t>(__builtin_clzl(static_cast<unsigned long>(mask))));
}
BOOST_FORCEINLINE std::size_t find_msb(unsigned long long mask, std::integral_constant<int, 3> const&)
{
   return static_cast<std::size_t>(static_cast<std::size_t>(sizeof(unsigned long long) * static_cast<std::size_t>(CHAR_BIT) - 1u) - static_cast<std::size_t>(__builtin_clzll(static_cast<unsigned long long>(mask))));
}
#ifdef BOOST_HAS_INT128

BOOST_FORCEINLINE std::size_t find_msb(uint128_type mask, std::integral_constant<int, 0> const&)
{
   union
   {
      uint128_type    v;
      std::uint64_t sv[2];
   } val;
   val.v = mask;
#if BOOST_MP_ENDIAN_LITTLE_BYTE
   if (val.sv[1])
      return find_msb(val.sv[1], std::integral_constant<int, 3>()) + 64;
   return find_msb(val.sv[0], std::integral_constant<int, 3>());
#else
   if (val.sv[0])
      return find_msb(val.sv[0], std::integral_constant<int, 3>()) + 64;
   return find_msb(val.sv[1], std::integral_constant<int, 3>());
#endif
}
BOOST_FORCEINLINE std::size_t find_lsb(uint128_type mask, std::integral_constant<int, 0> const&)
{
   union
   {
      uint128_type    v;
      std::uint64_t sv[2];
   } val;
   val.v = mask;
#if BOOST_MP_ENDIAN_LITTLE_BYTE
   if (val.sv[0] == 0)
      return find_lsb(val.sv[1], std::integral_constant<int, 3>()) + 64;
   return find_lsb(val.sv[0], std::integral_constant<int, 3>());
#else
   if (val.sv[1] == 0)
      return find_lsb(val.sv[0], std::integral_constant<int, 3>()) + 64;
   return find_lsb(val.sv[1], std::integral_constant<int, 3>());
#endif
}
#endif

template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned),
       std::integral_constant<int, 1>,
       typename std::conditional<
           sizeof(Unsigned) <= sizeof(unsigned long),
           std::integral_constant<int, 2>,
           typename std::conditional<
               sizeof(Unsigned) <= sizeof(unsigned long long),
               std::integral_constant<int, 3>,
               std::integral_constant<int, 0> >::type>::type>::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_lsb_default(mask);
   }
   else
#endif
   return find_lsb(static_cast<ui_type>(mask), tag_type());
}
template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned),
       std::integral_constant<int, 1>,
       typename std::conditional<
           sizeof(Unsigned) <= sizeof(unsigned long),
           std::integral_constant<int, 2>,
           typename std::conditional<
               sizeof(Unsigned) <= sizeof(unsigned long long),
               std::integral_constant<int, 3>,
               std::integral_constant<int, 0> >::type>::type>::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_msb_default(mask);
   }
   else
#endif
      return find_msb(static_cast<ui_type>(mask), tag_type());
}
#elif defined(BOOST_INTEL)
BOOST_FORCEINLINE std::size_t find_lsb(std::size_t mask, std::integral_constant<int, 1> const&)
{
   return _bit_scan_forward(mask);
}
BOOST_FORCEINLINE std::size_t find_msb(std::size_t mask, std::integral_constant<int, 1> const&)
{
   return _bit_scan_reverse(mask);
}
template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned),
       std::integral_constant<int, 1>,
       std::integral_constant<int, 0> >::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_lsb_default(mask);
   }
   else
#endif
      return find_lsb(static_cast<ui_type>(mask), tag_type());
}
template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb(Unsigned mask)
{
   using ui_type = typename boost::multiprecision::detail::make_unsigned<Unsigned>::type;
   using tag_type = typename std::conditional<
       sizeof(Unsigned) <= sizeof(unsigned),
       std::integral_constant<int, 1>,
       std::integral_constant<int, 0> >::type;
#ifndef BOOST_MP_NO_CONSTEXPR_DETECTION
   if (BOOST_MP_IS_CONST_EVALUATED(mask))
   {
      return find_msb_default(mask);
   }
   else
#endif
      return find_msb(static_cast<ui_type>(mask), tag_type());
}
#else
template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_lsb(Unsigned mask)
{
   return find_lsb(mask, std::integral_constant<int, 0>());
}
template <class Unsigned>
BOOST_FORCEINLINE BOOST_MP_CXX14_CONSTEXPR std::size_t find_msb(Unsigned mask)
{
   return find_msb(mask, std::integral_constant<int, 0>());
}
#endif

}}} // namespace boost::multiprecision::detail

#endif
