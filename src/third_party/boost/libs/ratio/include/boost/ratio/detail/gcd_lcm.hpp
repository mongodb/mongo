#ifndef BOOST_RATIO_DETAIL_GCD_LCM_HPP
#define BOOST_RATIO_DETAIL_GCD_LCM_HPP

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <type_traits>
#include <cstdint>

namespace boost
{
namespace ratio_detail
{

template<std::intmax_t A> struct abs_: std::integral_constant<std::intmax_t, A < 0? -A: A>
{
};

template<> struct abs_<INTMAX_MIN>: std::integral_constant<std::intmax_t, INTMAX_MIN>
{
};

template<std::intmax_t A, std::intmax_t B> struct gcd_: public gcd_<B, A % B>
{
};

template<std::intmax_t A> struct gcd_<A, 0>: std::integral_constant<std::intmax_t, A>
{
};

template<std::intmax_t A, std::intmax_t B> struct lcm_: std::integral_constant<std::intmax_t, (A / gcd_<A, B>::value) * B>
{
};

template<> struct lcm_<0, 0>: std::integral_constant<std::intmax_t, 0>
{
};

//

template<std::intmax_t A, std::intmax_t B> struct gcd: abs_< gcd_<A, B>::value >
{
};

template<std::intmax_t A, std::intmax_t B> struct lcm: abs_< lcm_<A, B>::value >
{
};

} // namespace ratio_detail
} // namespace boost

#endif // BOOST_RATIO_DETAIL_GCD_LCM_HPP
