///////////////////////////////////////////////////////////////
//  Copyright 2012 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_DETAIL_DIGITS_HPP
#define BOOST_MP_DETAIL_DIGITS_HPP

namespace boost { namespace multiprecision { namespace detail {

inline constexpr unsigned long digits10_2_2(unsigned long d10)
{
   return (d10 * 1000uL) / 301uL + ((d10 * 1000uL) % 301 ? 2u : 1u);
}

inline constexpr unsigned long digits2_2_10(unsigned long d2)
{
   return (d2 * 301uL) / 1000uL;
}


#if ULONG_MAX != SIZE_MAX

inline constexpr std::size_t digits10_2_2(std::size_t d10)
{
   return (d10 * 1000uL) / 301uL + ((d10 * 1000uL) % 301 ? 2u : 1u);
}

inline constexpr std::size_t digits2_2_10(std::size_t d2)
{
   return (d2 * 301uL) / 1000uL;
}

template <class I>
inline constexpr typename std::enable_if<sizeof(I) <= sizeof(unsigned long), unsigned long>::type digits10_2_2(I d10)
{
   return digits10_2_2(static_cast<unsigned long>(d10));
}

template <class I>
inline constexpr typename std::enable_if<sizeof(I) <= sizeof(unsigned long), unsigned long>::type digits2_2_10(I d10)
{
   return digits2_2_10(static_cast<unsigned long>(d10));
}
#endif

}}} // namespace boost::multiprecision::detail

#endif
