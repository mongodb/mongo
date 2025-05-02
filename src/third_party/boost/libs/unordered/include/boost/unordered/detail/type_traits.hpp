// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_TYPE_TRAITS_HPP
#define BOOST_UNORDERED_DETAIL_TYPE_TRAITS_HPP

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/config/workaround.hpp>

#if !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)
#include <iterator>
#endif

#include <type_traits>
#include <utility>

// BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES

#if !defined(BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES)
#if !defined(BOOST_NO_CXX17_DEDUCTION_GUIDES)
#define BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES 1
#endif
#endif

#if !defined(BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES)
#define BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES 0
#endif

namespace boost {
  namespace unordered {
    namespace detail {

      template <class T> struct type_identity
      {
        using type = T;
      };

      template <typename... Ts> struct make_void
      {
        typedef void type;
      };

      template <typename... Ts> using void_t = typename make_void<Ts...>::type;

      template <class T, class = void> struct is_complete : std::false_type
      {
      };

      template <class T>
      struct is_complete<T, void_t<int[sizeof(T)]> > : std::true_type
      {
      };

      template <class T>
      using is_complete_and_move_constructible =
        typename std::conditional<is_complete<T>::value,
          std::is_move_constructible<T>, std::false_type>::type;

#if BOOST_WORKAROUND(BOOST_LIBSTDCXX_VERSION, < 50000)
      /* std::is_trivially_default_constructible not provided */
      template <class T>
      struct is_trivially_default_constructible
          : public std::integral_constant<bool,
              std::is_default_constructible<T>::value &&
                std::has_trivial_default_constructor<T>::value>
      {
      };
#else
      using std::is_trivially_default_constructible;
#endif

#if BOOST_WORKAROUND(BOOST_LIBSTDCXX_VERSION, < 50000)
      /* std::is_trivially_copy_constructible not provided */
      template <class T>
      struct is_trivially_copy_constructible
          : public std::integral_constant<bool,
              std::is_copy_constructible<T>::value &&
                std::has_trivial_copy_constructor<T>::value>
      {
      };
#else
      using std::is_trivially_copy_constructible;
#endif

#if BOOST_WORKAROUND(BOOST_LIBSTDCXX_VERSION, < 50000)
      /* std::is_trivially_copy_assignable not provided */
      template <class T>
      struct is_trivially_copy_assignable
          : public std::integral_constant<bool,
              std::is_copy_assignable<T>::value &&
                std::has_trivial_copy_assign<T>::value>
      {
      };
#else
      using std::is_trivially_copy_assignable;
#endif

      namespace type_traits_detail {
        using std::swap;

        template <class T, class = void> struct is_nothrow_swappable_helper
        {
          constexpr static bool const value = false;
        };

        template <class T>
        struct is_nothrow_swappable_helper<T,
          void_t<decltype(swap(std::declval<T&>(), std::declval<T&>()))> >
        {
          constexpr static bool const value =
            noexcept(swap(std::declval<T&>(), std::declval<T&>()));
        };

      } // namespace type_traits_detail

      template <class T>
      struct is_nothrow_swappable
          : public std::integral_constant<bool,
              type_traits_detail::is_nothrow_swappable_helper<T>::value>
      {
      };

      ////////////////////////////////////////////////////////////////////////////
      // Type checkers used for the transparent member functions added by C++20
      // and up

      template <class, class = void>
      struct is_transparent : public std::false_type
      {
      };

      template <class T>
      struct is_transparent<T,
        boost::unordered::detail::void_t<typename T::is_transparent> >
          : public std::true_type
      {
      };

      template <class, class Hash, class KeyEqual> struct are_transparent
      {
        static bool const value =
          is_transparent<Hash>::value && is_transparent<KeyEqual>::value;
      };

      template <class Key, class UnorderedMap> struct transparent_non_iterable
      {
        typedef typename UnorderedMap::hasher hash;
        typedef typename UnorderedMap::key_equal key_equal;
        typedef typename UnorderedMap::iterator iterator;
        typedef typename UnorderedMap::const_iterator const_iterator;

        static bool const value =
          are_transparent<Key, hash, key_equal>::value &&
          !std::is_convertible<Key, iterator>::value &&
          !std::is_convertible<Key, const_iterator>::value;
      };

      template <class T>
      using remove_cvref_t =
        typename std::remove_cv<typename std::remove_reference<T>::type>::type;

      template <class T, class U>
      using is_similar = std::is_same<remove_cvref_t<T>, remove_cvref_t<U> >;

      template <class, class...> struct is_similar_to_any : std::false_type
      {
      };
      template <class T, class U, class... Us>
      struct is_similar_to_any<T, U, Us...>
          : std::conditional<is_similar<T, U>::value, is_similar<T, U>,
              is_similar_to_any<T, Us...> >::type
      {
      };

#if BOOST_UNORDERED_TEMPLATE_DEDUCTION_GUIDES
      // https://eel.is/c++draft/container.requirements#container.alloc.reqmts-34
      // https://eel.is/c++draft/container.requirements#unord.req.general-243

      template <class InputIterator>
      constexpr bool const is_input_iterator_v =
        !std::is_integral<InputIterator>::value;

      template <class A, class = void> struct is_allocator
      {
        constexpr static bool const value = false;
      };

      template <class A>
      struct is_allocator<A,
        boost::unordered::detail::void_t<typename A::value_type,
          decltype(std::declval<A&>().allocate(std::size_t{}))> >
      {
        constexpr static bool const value = true;
      };

      template <class A>
      constexpr bool const is_allocator_v = is_allocator<A>::value;

      template <class H>
      constexpr bool const is_hash_v =
        !std::is_integral<H>::value && !is_allocator_v<H>;

      template <class P> constexpr bool const is_pred_v = !is_allocator_v<P>;

      template <typename T>
      using iter_key_t =
        typename std::iterator_traits<T>::value_type::first_type;
      template <typename T>
      using iter_val_t =
        typename std::iterator_traits<T>::value_type::second_type;
      template <typename T>
      using iter_to_alloc_t =
        typename std::pair<iter_key_t<T> const, iter_val_t<T> >;
#endif

#if BOOST_CXX_VERSION < 201703L
      template <class T>
      constexpr typename std::add_const<T>::type& as_const(T& t) noexcept
      {
        return t;
      }
      template <class T> void as_const(const T&&) = delete;
#else
      using std::as_const;
#endif
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_TYPE_TRAITS_HPP
