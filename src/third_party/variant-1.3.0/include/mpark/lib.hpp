// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#ifndef MPARK_LIB_HPP
#define MPARK_LIB_HPP

#include <memory>
#include <functional>
#include <type_traits>
#include <utility>

#include "config.hpp"

#define RETURN(...)                                          \
  noexcept(noexcept(__VA_ARGS__)) -> decltype(__VA_ARGS__) { \
    return __VA_ARGS__;                                      \
  }

namespace mpark {
  namespace lib {
    template <typename T>
    struct identity { using type = T; };

    inline namespace cpp14 {
      template <typename T, std::size_t N>
      struct array {
        constexpr const T &operator[](std::size_t index) const {
          return data[index];
        }

        T data[N == 0 ? 1 : N];
      };

      template <typename T>
      using add_pointer_t = typename std::add_pointer<T>::type;

      template <typename... Ts>
      using common_type_t = typename std::common_type<Ts...>::type;

      template <typename T>
      using decay_t = typename std::decay<T>::type;

      template <bool B, typename T = void>
      using enable_if_t = typename std::enable_if<B, T>::type;

      template <typename T>
      using remove_const_t = typename std::remove_const<T>::type;

      template <typename T>
      using remove_reference_t = typename std::remove_reference<T>::type;

      template <typename T>
      inline constexpr T &&forward(remove_reference_t<T> &t) noexcept {
        return static_cast<T &&>(t);
      }

      template <typename T>
      inline constexpr T &&forward(remove_reference_t<T> &&t) noexcept {
        static_assert(!std::is_lvalue_reference<T>::value,
                      "can not forward an rvalue as an lvalue");
        return static_cast<T &&>(t);
      }

      template <typename T>
      inline constexpr remove_reference_t<T> &&move(T &&t) noexcept {
        return static_cast<remove_reference_t<T> &&>(t);
      }

#ifdef MPARK_INTEGER_SEQUENCE
      using std::integer_sequence;
      using std::index_sequence;
      using std::make_index_sequence;
      using std::index_sequence_for;
#else
      template <typename T, T... Is>
      struct integer_sequence {
        using value_type = T;
        static constexpr std::size_t size() noexcept { return sizeof...(Is); }
      };

      template <std::size_t... Is>
      using index_sequence = integer_sequence<std::size_t, Is...>;

      template <typename Lhs, typename Rhs>
      struct make_index_sequence_concat;

      template <std::size_t... Lhs, std::size_t... Rhs>
      struct make_index_sequence_concat<index_sequence<Lhs...>,
                                        index_sequence<Rhs...>>
          : identity<index_sequence<Lhs..., (sizeof...(Lhs) + Rhs)...>> {};

      template <std::size_t N>
      struct make_index_sequence_impl;

      template <std::size_t N>
      using make_index_sequence = typename make_index_sequence_impl<N>::type;

      template <std::size_t N>
      struct make_index_sequence_impl
          : make_index_sequence_concat<make_index_sequence<N / 2>,
                                       make_index_sequence<N - (N / 2)>> {};

      template <>
      struct make_index_sequence_impl<0> : identity<index_sequence<>> {};

      template <>
      struct make_index_sequence_impl<1> : identity<index_sequence<0>> {};

      template <typename... Ts>
      using index_sequence_for = make_index_sequence<sizeof...(Ts)>;
#endif

      // <functional>
#ifdef MPARK_TRANSPARENT_OPERATORS
      using equal_to = std::equal_to<>;
#else
      struct equal_to {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) == lib::forward<Rhs>(rhs))
      };
#endif

#ifdef MPARK_TRANSPARENT_OPERATORS
      using not_equal_to = std::not_equal_to<>;
#else
      struct not_equal_to {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) != lib::forward<Rhs>(rhs))
      };
#endif

#ifdef MPARK_TRANSPARENT_OPERATORS
      using less = std::less<>;
#else
      struct less {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) < lib::forward<Rhs>(rhs))
      };
#endif

#ifdef MPARK_TRANSPARENT_OPERATORS
      using greater = std::greater<>;
#else
      struct greater {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) > lib::forward<Rhs>(rhs))
      };
#endif

#ifdef MPARK_TRANSPARENT_OPERATORS
      using less_equal = std::less_equal<>;
#else
      struct less_equal {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) <= lib::forward<Rhs>(rhs))
      };
#endif

#ifdef MPARK_TRANSPARENT_OPERATORS
      using greater_equal = std::greater_equal<>;
#else
      struct greater_equal {
        template <typename Lhs, typename Rhs>
        inline constexpr auto operator()(Lhs &&lhs, Rhs &&rhs) const
          RETURN(lib::forward<Lhs>(lhs) >= lib::forward<Rhs>(rhs))
      };
#endif
    }  // namespace cpp14

    inline namespace cpp17 {

      // <type_traits>
      template <bool B>
      using bool_constant = std::integral_constant<bool, B>;

      template <typename...>
      struct voider : identity<void> {};

      template <typename... Ts>
      using void_t = typename voider<Ts...>::type;

      namespace detail {
        namespace swappable {

          using std::swap;

          template <typename T>
          struct is_swappable {
            private:
            template <typename U,
                      typename = decltype(swap(std::declval<U &>(),
                                               std::declval<U &>()))>
            inline static std::true_type test(int);

            template <typename U>
            inline static std::false_type test(...);

            public:
            static constexpr bool value = decltype(test<T>(0))::value;
          };

          template <typename T, bool = is_swappable<T>::value>
          struct is_nothrow_swappable {
            static constexpr bool value =
                noexcept(swap(std::declval<T &>(), std::declval<T &>()));
          };

          template <typename T>
          struct is_nothrow_swappable<T, false> : std::false_type {};

        }  // namespace swappable
      }  // namespace detail

      using detail::swappable::is_swappable;
      using detail::swappable::is_nothrow_swappable;

      // <functional>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
      template <typename F, typename... As>
      inline constexpr auto invoke(F &&f, As &&... as)
          RETURN(lib::forward<F>(f)(lib::forward<As>(as)...))
#ifdef _MSC_VER
#pragma warning(pop)
#endif

      template <typename B, typename T, typename D>
      inline constexpr auto invoke(T B::*pmv, D &&d)
          RETURN(lib::forward<D>(d).*pmv)

      template <typename Pmv, typename Ptr>
      inline constexpr auto invoke(Pmv pmv, Ptr &&ptr)
          RETURN((*lib::forward<Ptr>(ptr)).*pmv)

      template <typename B, typename T, typename D, typename... As>
      inline constexpr auto invoke(T B::*pmf, D &&d, As &&... as)
          RETURN((lib::forward<D>(d).*pmf)(lib::forward<As>(as)...))

      template <typename Pmf, typename Ptr, typename... As>
      inline constexpr auto invoke(Pmf pmf, Ptr &&ptr, As &&... as)
          RETURN(((*lib::forward<Ptr>(ptr)).*pmf)(lib::forward<As>(as)...))

      namespace detail {

        template <typename Void, typename, typename...>
        struct invoke_result {};

        template <typename F, typename... Args>
        struct invoke_result<void_t<decltype(lib::invoke(
                                 std::declval<F>(), std::declval<Args>()...))>,
                             F,
                             Args...>
            : identity<decltype(
                  lib::invoke(std::declval<F>(), std::declval<Args>()...))> {};

      }  // namespace detail

      template <typename F, typename... Args>
      using invoke_result = detail::invoke_result<void, F, Args...>;

      template <typename F, typename... Args>
      using invoke_result_t = typename invoke_result<F, Args...>::type;

      namespace detail {

        template <typename Void, typename, typename...>
        struct is_invocable : std::false_type {};

        template <typename F, typename... Args>
        struct is_invocable<void_t<invoke_result_t<F, Args...>>, F, Args...>
            : std::true_type {};

        template <typename Void, typename, typename, typename...>
        struct is_invocable_r : std::false_type {};

        template <typename R, typename F, typename... Args>
        struct is_invocable_r<void_t<invoke_result_t<F, Args...>>,
                              R,
                              F,
                              Args...>
            : std::is_convertible<invoke_result_t<F, Args...>, R> {};

      }  // namespace detail

      template <typename F, typename... Args>
      using is_invocable = detail::is_invocable<void, F, Args...>;

      template <typename R, typename F, typename... Args>
      using is_invocable_r = detail::is_invocable_r<void, R, F, Args...>;

      // <memory>
#ifdef MPARK_BUILTIN_ADDRESSOF
      template <typename T>
      inline constexpr T *addressof(T &arg) {
        return __builtin_addressof(arg);
      }
#else
      namespace detail {

        namespace has_addressof_impl {

          struct fail;

          template <typename T>
          inline fail operator&(T &&);

          template <typename T>
          inline static constexpr bool impl() {
            return (std::is_class<T>::value || std::is_union<T>::value) &&
                   !std::is_same<decltype(&std::declval<T &>()), fail>::value;
          }

        }  // namespace has_addressof_impl

        template <typename T>
        using has_addressof = bool_constant<has_addressof_impl::impl<T>()>;

        template <typename T>
        inline constexpr T *addressof(T &arg, std::true_type) {
          return std::addressof(arg);
        }

        template <typename T>
        inline constexpr T *addressof(T &arg, std::false_type) {
          return &arg;
        }

      }  // namespace detail

      template <typename T>
      inline constexpr T *addressof(T &arg) {
        return detail::addressof(arg, detail::has_addressof<T>{});
      }
#endif

      template <typename T>
      inline constexpr T *addressof(const T &&) = delete;

    }  // namespace cpp17

    template <typename T>
    struct remove_all_extents : identity<T> {};

    template <typename T, std::size_t N>
    struct remove_all_extents<array<T, N>> : remove_all_extents<T> {};

    template <typename T>
    using remove_all_extents_t = typename remove_all_extents<T>::type;

    template <std::size_t N>
    using size_constant = std::integral_constant<std::size_t, N>;

    template <std::size_t I, typename T>
    struct indexed_type : size_constant<I>, identity<T> {};

    template <bool... Bs>
    using all = std::is_same<integer_sequence<bool, true, Bs...>,
                             integer_sequence<bool, Bs..., true>>;

#ifdef MPARK_TYPE_PACK_ELEMENT
    template <std::size_t I, typename... Ts>
    using type_pack_element_t = __type_pack_element<I, Ts...>;
#else
    template <std::size_t I, typename... Ts>
    struct type_pack_element_impl {
      private:
      template <typename>
      struct set;

      template <std::size_t... Is>
      struct set<index_sequence<Is...>> : indexed_type<Is, Ts>... {};

      template <typename T>
      inline static std::enable_if<true, T> impl(indexed_type<I, T>);

      inline static std::enable_if<false> impl(...);

      public:
      using type = decltype(impl(set<index_sequence_for<Ts...>>{}));
    };

    template <std::size_t I, typename... Ts>
    using type_pack_element = typename type_pack_element_impl<I, Ts...>::type;

    template <std::size_t I, typename... Ts>
    using type_pack_element_t = typename type_pack_element<I, Ts...>::type;
#endif

#ifdef MPARK_TRIVIALITY_TYPE_TRAITS
    using std::is_trivially_copy_constructible;
    using std::is_trivially_move_constructible;
    using std::is_trivially_copy_assignable;
    using std::is_trivially_move_assignable;
#else
    template <typename T>
    struct is_trivially_copy_constructible
        : bool_constant<
              std::is_copy_constructible<T>::value && __has_trivial_copy(T)> {};

    template <typename T>
    struct is_trivially_move_constructible : bool_constant<__is_trivial(T)> {};

    template <typename T>
    struct is_trivially_copy_assignable
        : bool_constant<
              std::is_copy_assignable<T>::value && __has_trivial_assign(T)> {};

    template <typename T>
    struct is_trivially_move_assignable : bool_constant<__is_trivial(T)> {};
#endif

    template <typename T, bool>
    struct dependent_type : T {};

    template <typename Is, std::size_t J>
    struct push_back;

    template <typename Is, std::size_t J>
    using push_back_t = typename push_back<Is, J>::type;

    template <std::size_t... Is, std::size_t J>
    struct push_back<index_sequence<Is...>, J> {
      using type = index_sequence<Is..., J>;
    };

  }  // namespace lib
}  // namespace mpark

#undef RETURN

#endif  // MPARK_LIB_HPP
