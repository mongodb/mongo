/* Copyright 2023 Christian Mazakas.
 * Copyright 2023-2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_CONCURRENT_STATIC_ASSERTS_HPP
#define BOOST_UNORDERED_DETAIL_CONCURRENT_STATIC_ASSERTS_HPP

#include <boost/config.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>
#include <boost/unordered/detail/type_traits.hpp>

#define BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(F)                             \
  static_assert(boost::unordered::detail::is_invocable<F, value_type&>::value, \
    "The provided Callable must be invocable with value_type&");

#define BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(F)                       \
  static_assert(                                                               \
    boost::unordered::detail::is_invocable<F, value_type const&>::value,       \
    "The provided Callable must be invocable with value_type const&");

#if BOOST_CXX_VERSION >= 202002L

#define BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(P)                           \
  static_assert(!std::is_base_of<std::execution::parallel_unsequenced_policy,  \
                  ExecPolicy>::value,                                          \
    "ExecPolicy must be sequenced.");                                          \
  static_assert(                                                               \
    !std::is_base_of<std::execution::unsequenced_policy, ExecPolicy>::value,   \
    "ExecPolicy must be sequenced.");

#else

#define BOOST_UNORDERED_STATIC_ASSERT_EXEC_POLICY(P)                           \
  static_assert(!std::is_base_of<std::execution::parallel_unsequenced_policy,  \
                  ExecPolicy>::value,                                          \
    "ExecPolicy must be sequenced.");
#endif

#define BOOST_UNORDERED_DETAIL_COMMA ,

#define BOOST_UNORDERED_DETAIL_LAST_ARG(Arg, Args)                             \
  mp11::mp_back<mp11::mp_list<Arg BOOST_UNORDERED_DETAIL_COMMA Args> >

#define BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_INVOCABLE(Arg, Args)            \
  BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(                                     \
    BOOST_UNORDERED_DETAIL_LAST_ARG(Arg, Args))

#define BOOST_UNORDERED_STATIC_ASSERT_LAST_ARG_CONST_INVOCABLE(Arg, Args)      \
  BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(                               \
    BOOST_UNORDERED_DETAIL_LAST_ARG(Arg, Args))

#define BOOST_UNORDERED_DETAIL_PENULTIMATE_ARG(Arg1, Arg2, Args)               \
  mp11::mp_at_c<mp11::mp_list<                                                 \
    Arg1 BOOST_UNORDERED_DETAIL_COMMA Arg2 BOOST_UNORDERED_DETAIL_COMMA Args   \
    >,                                                                         \
    mp11::mp_size<mp11::mp_list<                                               \
      Arg1 BOOST_UNORDERED_DETAIL_COMMA Arg2 BOOST_UNORDERED_DETAIL_COMMA Args \
    >>::value - 2>

#define BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_INVOCABLE(               \
  Arg1, Arg2, Args)                                                            \
  BOOST_UNORDERED_STATIC_ASSERT_INVOCABLE(                                     \
    BOOST_UNORDERED_DETAIL_PENULTIMATE_ARG(Arg1, Arg2, Args))

#define BOOST_UNORDERED_STATIC_ASSERT_PENULTIMATE_ARG_CONST_INVOCABLE(         \
  Arg1, Arg2, Args)                                                            \
  BOOST_UNORDERED_STATIC_ASSERT_CONST_INVOCABLE(                               \
    BOOST_UNORDERED_DETAIL_PENULTIMATE_ARG(Arg1, Arg2, Args))

namespace boost {
  namespace unordered {
    namespace detail {
      template <class...> struct is_invocable_helper : std::false_type
      {
      };

      template <class F, class... Args>
      struct is_invocable_helper<
        void_t<decltype(std::declval<F>()(std::declval<Args>()...))>, F,
        Args...> : std::true_type
      {
      };

      template <class F, class... Args>
      using is_invocable = is_invocable_helper<void, F, Args...>;

    } // namespace detail

  } // namespace unordered

} // namespace boost

#if defined(BOOST_NO_CXX20_HDR_CONCEPTS)
#define BOOST_UNORDERED_STATIC_ASSERT_FWD_ITERATOR(Iterator)                   \
  static_assert(                                                               \
    std::is_base_of<                                                           \
      std::forward_iterator_tag,                                               \
      typename std::iterator_traits<Iterator>::iterator_category>::value,      \
    "The provided iterator must be at least forward");
#else
#define BOOST_UNORDERED_STATIC_ASSERT_FWD_ITERATOR(Iterator)                   \
  static_assert(std::forward_iterator<Iterator>,                               \
    "The provided iterator must be at least forward");

#endif

#define BOOST_UNORDERED_STATIC_ASSERT_KEY_COMPATIBLE_ITERATOR(Iterator)        \
  static_assert(                                                               \
    std::is_same<                                                              \
      typename std::iterator_traits<Iterator>::value_type,                     \
      key_type>::value ||                                                      \
    detail::are_transparent<                                                   \
      typename std::iterator_traits<Iterator>::value_type,                     \
      hasher, key_equal>::value,                                               \
    "The provided iterator must dereference to a compatible key value");

#define BOOST_UNORDERED_STATIC_ASSERT_BULK_VISIT_ITERATOR(Iterator)            \
  BOOST_UNORDERED_STATIC_ASSERT_FWD_ITERATOR(Iterator)                         \
  BOOST_UNORDERED_STATIC_ASSERT_KEY_COMPATIBLE_ITERATOR(Iterator)

#endif // BOOST_UNORDERED_DETAIL_CONCURRENT_STATIC_ASSERTS_HPP
