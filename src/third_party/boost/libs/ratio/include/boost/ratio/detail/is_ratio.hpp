#ifndef BOOST_RATIO_DETAIL_IS_RATIO_HPP
#define BOOST_RATIO_DETAIL_IS_RATIO_HPP

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/ratio/ratio_fwd.hpp>
#include <type_traits>
#include <cstdint>

namespace boost
{
namespace ratio_detail
{

template<class T> struct is_ratio: std::false_type
{
};

template<std::intmax_t A, std::intmax_t B> struct is_ratio< boost::ratio<A, B> >: std::true_type
{
};

} // namespace ratio_detail
} // namespace boost

#endif // BOOST_RATIO_DETAIL_IS_RATIO_HPP
