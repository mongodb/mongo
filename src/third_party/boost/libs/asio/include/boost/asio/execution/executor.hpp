//
// execution/executor.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_EXECUTION_EXECUTOR_HPP
#define BOOST_ASIO_EXECUTION_EXECUTOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/execution/invocable_archetype.hpp>
#include <boost/asio/traits/equality_comparable.hpp>
#include <boost/asio/traits/execute_member.hpp>

#if defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT) \
  && defined(BOOST_ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)
# define BOOST_ASIO_HAS_DEDUCED_EXECUTION_IS_EXECUTOR_TRAIT 1
#endif // defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)
       //   && defined(BOOST_ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {
namespace execution {
namespace detail {

template <typename T, typename F,
    typename = void, typename = void, typename = void, typename = void,
    typename = void, typename = void, typename = void, typename = void>
struct is_executor_of_impl : false_type
{
};

template <typename T, typename F>
struct is_executor_of_impl<T, F,
  enable_if_t<
    traits::execute_member<add_const_t<T>, F>::is_valid
  >,
  void_t<
    result_of_t<decay_t<F>&()>
  >,
  enable_if_t<
    is_constructible<decay_t<F>, F>::value
  >,
  enable_if_t<
    is_move_constructible<decay_t<F>>::value
  >,
  enable_if_t<
    is_nothrow_copy_constructible<T>::value
  >,
  enable_if_t<
    is_nothrow_destructible<T>::value
  >,
  enable_if_t<
    traits::equality_comparable<T>::is_valid
  >,
  enable_if_t<
    traits::equality_comparable<T>::is_noexcept
  >> : true_type
{
};

} // namespace detail

/// The is_executor trait detects whether a type T satisfies the
/// execution::executor concept.
/**
 * Class template @c is_executor is a UnaryTypeTrait that is derived from @c
 * true_type if the type @c T meets the concept definition for an executor,
 * otherwise @c false_type.
 */
template <typename T>
struct is_executor :
#if defined(GENERATING_DOCUMENTATION)
  integral_constant<bool, automatically_determined>
#else // defined(GENERATING_DOCUMENTATION)
  detail::is_executor_of_impl<T, invocable_archetype>
#endif // defined(GENERATING_DOCUMENTATION)
{
};

#if defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES)

template <typename T>
constexpr const bool is_executor_v = is_executor<T>::value;

#endif // defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES)

#if defined(BOOST_ASIO_HAS_CONCEPTS)

template <typename T>
BOOST_ASIO_CONCEPT executor = is_executor<T>::value;

#define BOOST_ASIO_EXECUTION_EXECUTOR ::boost::asio::execution::executor

#else // defined(BOOST_ASIO_HAS_CONCEPTS)

#define BOOST_ASIO_EXECUTION_EXECUTOR typename

#endif // defined(BOOST_ASIO_HAS_CONCEPTS)

} // namespace execution
} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // BOOST_ASIO_EXECUTION_EXECUTOR_HPP
