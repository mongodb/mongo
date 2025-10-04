// Copyright (C) 2024 Braden Ganetsky
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_FOA_TYPES_CONSTRUCTIBILITY_HPP
#define BOOST_UNORDERED_DETAIL_FOA_TYPES_CONSTRUCTIBILITY_HPP

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace boost {
  namespace unordered {
    namespace detail {
      namespace foa {
        template <class Key, class... Args> struct check_key_type_t
        {
          static_assert(std::is_constructible<Key, Args...>::value,
            "key_type must be constructible from Args");
        };
        template <class Key> struct check_key_type_t<Key>
        {
          static_assert(std::is_constructible<Key>::value,
            "key_type must be default constructible");
        };
        template <class Key> struct check_key_type_t<Key, const Key&>
        {
          static_assert(std::is_constructible<Key, const Key&>::value,
            "key_type must be copy constructible");
        };
        template <class Key> struct check_key_type_t<Key, Key&&>
        {
          static_assert(std::is_constructible<Key, Key&&>::value,
            "key_type must be move constructible");
        };

        template <class Mapped, class... Args> struct check_mapped_type_t
        {
          static_assert(std::is_constructible<Mapped, Args...>::value,
            "mapped_type must be constructible from Args");
        };
        template <class Mapped> struct check_mapped_type_t<Mapped>
        {
          static_assert(std::is_constructible<Mapped>::value,
            "mapped_type must be default constructible");
        };
        template <class Mapped>
        struct check_mapped_type_t<Mapped, const Mapped&>
        {
          static_assert(std::is_constructible<Mapped, const Mapped&>::value,
            "mapped_type must be copy constructible");
        };
        template <class Mapped> struct check_mapped_type_t<Mapped, Mapped&&>
        {
          static_assert(std::is_constructible<Mapped, Mapped&&>::value,
            "mapped_type must be move constructible");
        };

        template <class TypePolicy> struct map_types_constructibility
        {
          using key_type = typename TypePolicy::key_type;
          using mapped_type = typename TypePolicy::mapped_type;
          using init_type = typename TypePolicy::init_type;
          using value_type = typename TypePolicy::value_type;

          template <class A, class X, class... Args>
          static void check(A&, X*, Args&&...)
          {
            // Pass through, as we cannot say anything about a general allocator
          }

          template <class... Args> static void check_key_type()
          {
            (void)check_key_type_t<key_type, Args...>{};
          }
          template <class... Args> static void check_mapped_type()
          {
            (void)check_mapped_type_t<mapped_type, Args...>{};
          }

          template <class Arg>
          static void check(std::allocator<value_type>&, key_type*, Arg&&)
          {
            check_key_type<Arg&&>();
          }

          template <class Arg1, class Arg2>
          static void check(
            std::allocator<value_type>&, value_type*, Arg1&&, Arg2&&)
          {
            check_key_type<Arg1&&>();
            check_mapped_type<Arg2&&>();
          }
          template <class Arg1, class Arg2>
          static void check(std::allocator<value_type>&, value_type*,
            const std::pair<Arg1, Arg2>&)
          {
            check_key_type<const Arg1&>();
            check_mapped_type<const Arg2&>();
          }
          template <class Arg1, class Arg2>
          static void check(
            std::allocator<value_type>&, value_type*, std::pair<Arg1, Arg2>&&)
          {
            check_key_type<Arg1&&>();
            check_mapped_type<Arg2&&>();
          }
          template <class... Args1, class... Args2>
          static void check(std::allocator<value_type>&, value_type*,
            std::piecewise_construct_t, std::tuple<Args1...>&&,
            std::tuple<Args2...>&&)
          {
            check_key_type<Args1&&...>();
            check_mapped_type<Args2&&...>();
          }

          template <class Arg1, class Arg2>
          static void check(
            std::allocator<value_type>&, init_type*, Arg1&&, Arg2&&)
          {
            check_key_type<Arg1&&>();
            check_mapped_type<Arg2&&>();
          }
          template <class Arg1, class Arg2>
          static void check(std::allocator<value_type>&, init_type*,
            const std::pair<Arg1, Arg2>&)
          {
            check_key_type<const Arg1&>();
            check_mapped_type<const Arg2&>();
          }
          template <class Arg1, class Arg2>
          static void check(
            std::allocator<value_type>&, init_type*, std::pair<Arg1, Arg2>&&)
          {
            check_key_type<Arg1&&>();
            check_mapped_type<Arg2&&>();
          }
          template <class... Args1, class... Args2>
          static void check(std::allocator<value_type>&, init_type*,
            std::piecewise_construct_t, std::tuple<Args1...>&&,
            std::tuple<Args2...>&&)
          {
            check_key_type<Args1&&...>();
            check_mapped_type<Args2&&...>();
          }
        };

        template <class TypePolicy> struct set_types_constructibility
        {
          using key_type = typename TypePolicy::key_type;
          using value_type = typename TypePolicy::value_type;
          static_assert(std::is_same<key_type, value_type>::value, "");

          template <class A, class X, class... Args>
          static void check(A&, X*, Args&&...)
          {
            // Pass through, as we cannot say anything about a general allocator
          }

          template <class... Args>
          static void check(std::allocator<value_type>&, key_type*, Args&&...)
          {
            (void)check_key_type_t<key_type, Args&&...>{};
          }
        };
      } // namespace foa
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif // BOOST_UNORDERED_DETAIL_FOA_TYPES_CONSTRUCTIBILITY_HPP
