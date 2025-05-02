#ifndef BOOST_RATIO_DETAIL_IS_EVENLY_DIVISIBLE_BY_HPP
#define BOOST_RATIO_DETAIL_IS_EVENLY_DIVISIBLE_BY_HPP

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <type_traits>
#include <cstdint>

namespace boost
{
namespace ratio_detail
{

template<std::intmax_t A, std::intmax_t B> struct is_evenly_divisible_by_: std::integral_constant<bool, A % B == 0>
{
};

template<std::intmax_t A> struct is_evenly_divisible_by_<A, 0>: std::false_type
{
};

template<class R1, class R2> struct is_evenly_divisible_by: std::integral_constant<bool,
    is_evenly_divisible_by_<R1::num, R2::num>::value && is_evenly_divisible_by_<R2::den, R1::den>::value>
{
};

} // namespace ratio_detail
} // namespace boost

#endif // BOOST_RATIO_DETAIL_IS_EVENLY_DIVISIBLE_BY_HPP
