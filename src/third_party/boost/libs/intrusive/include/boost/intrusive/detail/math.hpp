/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_MATH_HPP
#define BOOST_INTRUSIVE_DETAIL_MATH_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <cstddef>
#include <climits>
#include <boost/intrusive/detail/mpl.hpp>
#include <cstring>

namespace boost {
namespace intrusive {
namespace detail {

///////////////////////////
// floor_log2  Dispatcher
////////////////////////////

#if defined(_MSC_VER) && (_MSC_VER >= 1300)

   }}} //namespace boost::intrusive::detail

   //Use _BitScanReverseXX intrinsics

   #if defined(_M_X64) || defined(_M_AMD64) || defined(_M_IA64)   //64 bit target
      #define BOOST_INTRUSIVE_BSR_INTRINSIC_64_BIT
   #endif

   #ifndef __INTRIN_H_   // Avoid including any windows system header
      #ifdef __cplusplus
      extern "C" {
      #endif // __cplusplus

      #if defined(BOOST_INTRUSIVE_BSR_INTRINSIC_64_BIT)   //64 bit target
         unsigned char _BitScanReverse64(unsigned long *index, unsigned __int64 mask);
         #pragma intrinsic(_BitScanReverse64)
      #else //32 bit target
         unsigned char _BitScanReverse(unsigned long *index, unsigned long mask);
         #pragma intrinsic(_BitScanReverse)
      #endif

      #ifdef __cplusplus
      }
      #endif // __cplusplus
   #endif // __INTRIN_H_

   #ifdef BOOST_INTRUSIVE_BSR_INTRINSIC_64_BIT
      #define BOOST_INTRUSIVE_BSR_INTRINSIC _BitScanReverse64
      #undef BOOST_INTRUSIVE_BSR_INTRINSIC_64_BIT
   #else
      #define BOOST_INTRUSIVE_BSR_INTRINSIC _BitScanReverse
   #endif

   namespace boost {
   namespace intrusive {
   namespace detail {

   inline std::size_t floor_log2 (std::size_t x)
   {
      unsigned long log2;
      BOOST_INTRUSIVE_BSR_INTRINSIC( &log2, x );
      return static_cast<std::size_t>(log2);
   }

   #undef BOOST_INTRUSIVE_BSR_INTRINSIC

#elif defined(__GNUC__) && ((__GNUC__ >= 4) || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4)) //GCC >=3.4

   //Compile-time error in case of missing specialization
   template<class Uint>
   struct builtin_clz_dispatch;

   #if defined(BOOST_HAS_LONG_LONG)
   template<>
   struct builtin_clz_dispatch< ::boost::ulong_long_type >
   {
      static ::boost::ulong_long_type call(::boost::ulong_long_type n)
      {  return (::boost::ulong_long_type)__builtin_clzll(n); }
   };
   #endif

   template<>
   struct builtin_clz_dispatch<unsigned long>
   {
      static unsigned long call(unsigned long n)
      {  return (unsigned long)__builtin_clzl(n); }
   };

   template<>
   struct builtin_clz_dispatch<unsigned int>
   {
      static unsigned int call(unsigned int n)
      {  return (unsigned int)__builtin_clz(n); }
   };

   inline std::size_t floor_log2(std::size_t n)
   {
      return sizeof(std::size_t)*CHAR_BIT - std::size_t(1) - builtin_clz_dispatch<std::size_t>::call(n);
   }

#else //Portable methods

////////////////////////////
// Generic method
////////////////////////////

   inline std::size_t floor_log2_get_shift(std::size_t n, true_ )//power of two size_t
   {  return n >> 1;  }

   inline std::size_t floor_log2_get_shift(std::size_t n, false_ )//non-power of two size_t
   {  return (n >> 1) + ((n & 1u) & (n != 1)); }

   template<std::size_t N>
   inline std::size_t floor_log2 (std::size_t x, integral_constant<std::size_t, N>)
   {
      const std::size_t Bits = N;
      const bool Size_t_Bits_Power_2= !(Bits & (Bits-1));

      std::size_t n = x;
      std::size_t log2 = 0;

      std::size_t remaining_bits = Bits;
      std::size_t shift = floor_log2_get_shift(remaining_bits, bool_<Size_t_Bits_Power_2>());
      while(shift){
         std::size_t tmp = n >> shift;
         if (tmp){
            log2 += shift, n = tmp;
         }
         shift = floor_log2_get_shift(shift, bool_<Size_t_Bits_Power_2>());
      }

      return log2;
   }

   inline std::size_t floor_log2 (std::size_t x)
   {
      const std::size_t Bits = sizeof(std::size_t)*CHAR_BIT;
      return floor_log2(x, integral_constant<std::size_t, Bits>());
   }

#endif

//Thanks to Laurent de Soras in
//http://www.flipcode.com/archives/Fast_log_Function.shtml
inline float fast_log2 (float val)
{
   unsigned x;
   std::memcpy(&x, &val, sizeof(float));
   const int log_2 = int((x >> 23) & 255) - 128;
   x &= ~(unsigned(255u) << 23u);
   x += unsigned(127) << 23u;
   std::memcpy(&val, &x, sizeof(float));
   //1+log2(m), m ranging from 1 to 2
   //3rd degree polynomial keeping first derivate continuity.
   //For less precision the line can be commented out
   val = ((-1.f/3.f) * val + 2.f) * val - (2.f/3.f);
   return val + static_cast<float>(log_2);
}

inline bool is_pow2(std::size_t x)
{  return (x & (x-1)) == 0;  }

template<std::size_t N>
struct static_is_pow2
{
   static const bool value = (N & (N-1)) == 0;
};

inline std::size_t ceil_log2 (std::size_t x)
{
   return static_cast<std::size_t>(!(is_pow2)(x)) + floor_log2(x);
}

inline std::size_t ceil_pow2 (std::size_t x)
{
   return std::size_t(1u) << (ceil_log2)(x);
}

inline std::size_t previous_or_equal_pow2(std::size_t x)
{
   return std::size_t(1u) << floor_log2(x);
}

template<class SizeType, std::size_t N>
struct numbits_eq
{
   static const bool value = sizeof(SizeType)*CHAR_BIT == N;
};

template<class SizeType, class Enabler = void >
struct sqrt2_pow_max;

template <class SizeType>
struct sqrt2_pow_max<SizeType, typename voider<typename enable_if< numbits_eq<SizeType, 32> >::type>::type>
{
   static const SizeType value = 0xb504f334;
   static const std::size_t pow   = 31;
};

#ifndef BOOST_NO_INT64_T

template <class SizeType>
struct sqrt2_pow_max<SizeType, typename voider<typename enable_if< numbits_eq<SizeType, 64> >::type>::type>
{
   static const SizeType value = 0xb504f333f9de6484ull;
   static const std::size_t pow   = 63;
};

#endif   //BOOST_NO_INT64_T

// Returns floor(pow(sqrt(2), x * 2 + 1)).
// Defined for X from 0 up to the number of bits in size_t minus 1.
inline std::size_t sqrt2_pow_2xplus1 (std::size_t x)
{
   const std::size_t value = (std::size_t)sqrt2_pow_max<std::size_t>::value;
   const std::size_t pow   = (std::size_t)sqrt2_pow_max<std::size_t>::pow;
   return (value >> (pow - x)) + 1;
}

} //namespace detail
} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DETAIL_MATH_HPP
