//
// traits/static_query.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_TRAITS_STATIC_QUERY_HPP
#define BOOST_ASIO_TRAITS_STATIC_QUERY_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/type_traits.hpp>

#if defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES) \
  && defined(BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE)
# define BOOST_ASIO_HAS_DEDUCED_STATIC_QUERY_TRAIT 1
#endif // defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES)
       //   && defined(BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace traits {

template <typename T, typename Property, typename = void>
struct static_query_default;

template <typename T, typename Property, typename = void>
struct static_query;

} // namespace traits
namespace detail {

struct no_static_query
{
  static constexpr bool is_valid = false;
  static constexpr bool is_noexcept = false;
};

template <typename T, typename Property, typename = void>
struct static_query_trait :
  conditional_t<
    is_same<T, decay_t<T>>::value
      && is_same<Property, decay_t<Property>>::value,
    no_static_query,
    traits::static_query<
      decay_t<T>,
      decay_t<Property>>
  >
{
};

#if defined(BOOST_ASIO_HAS_DEDUCED_STATIC_QUERY_TRAIT)

template <typename T, typename Property>
struct static_query_trait<T, Property,
  void_t<
    decltype(decay_t<Property>::template static_query_v<T>)
  >>
{
  static constexpr bool is_valid = true;

  using result_type = decltype(
      decay_t<Property>::template static_query_v<T>);

  static constexpr bool is_noexcept =
    noexcept(decay_t<Property>::template static_query_v<T>);

  static constexpr result_type value() noexcept(is_noexcept)
  {
    return decay_t<Property>::template static_query_v<T>;
  }
};

#endif // defined(BOOST_ASIO_HAS_DEDUCED_STATIC_QUERY_TRAIT)

} // namespace detail
namespace traits {

template <typename T, typename Property, typename>
struct static_query_default : detail::static_query_trait<T, Property>
{
};

template <typename T, typename Property, typename>
struct static_query : static_query_default<T, Property>
{
};

} // namespace traits
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_TRAITS_STATIC_QUERY_HPP
