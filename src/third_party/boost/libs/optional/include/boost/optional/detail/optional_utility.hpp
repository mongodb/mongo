// Copyright (C) 2024 Ryan Malcolm Underwood.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  typenametea@gmail.com

#ifndef BOOST_OPTIONAL_OPTIONAL_DETAIL_OPTIONAL_UTILITY_RMU_06OCT2024_HPP
#define BOOST_OPTIONAL_OPTIONAL_DETAIL_OPTIONAL_UTILITY_RMU_06OCT2024_HPP

namespace boost {
namespace optional_detail {

// Workaround: forward and move aren't constexpr in C++11
template <class T>
inline constexpr T&& forward(typename boost::remove_reference<T>::type& t) noexcept
{
  return static_cast<T&&>(t);
}

template <class T>
inline constexpr T&& forward(typename boost::remove_reference<T>::type&& t) noexcept
{
  static_assert(!boost::is_lvalue_reference<T>::value, "Can not forward an rvalue as an lvalue.");
  return static_cast<T&&>(t);
}

template <class T>
inline constexpr typename boost::remove_reference<T>::type&& move(T&& t) noexcept
{
  return static_cast<typename boost::remove_reference<T>::type&&>(t);
}

} // namespace optional_detail
} // namespace boost

#endif
