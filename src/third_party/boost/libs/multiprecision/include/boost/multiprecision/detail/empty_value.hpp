/////////////////////////////////////////////////////////////////////
//  Copyright 2018 Glen Joseph Fernandes. 
//  Copyright 2021 Matt Borland. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_DETAIL_EMPTY_VALUE_HPP
#define BOOST_MP_DETAIL_EMPTY_VALUE_HPP

#include <utility>
#include <boost/multiprecision/detail/standalone_config.hpp>

#if defined(BOOST_GCC_VERSION) && (BOOST_GCC_VERSION >= 40700)
#define BOOST_DETAIL_EMPTY_VALUE_BASE
#elif defined(BOOST_INTEL) && defined(_MSC_VER) && (_MSC_VER >= 1800)
#define BOOST_DETAIL_EMPTY_VALUE_BASE
#elif defined(BOOST_MSVC) && (BOOST_MSVC >= 1800)
#define BOOST_DETAIL_EMPTY_VALUE_BASE
#elif defined(BOOST_CLANG) && !defined(__CUDACC__)
#if __has_feature(is_empty) && __has_feature(is_final)
#define BOOST_DETAIL_EMPTY_VALUE_BASE
#endif
#endif

namespace boost { namespace multiprecision { namespace detail {

template <typename T>
struct use_empty_value_base 
{
#if defined(BOOST_DETAIL_EMPTY_VALUE_BASE)
        static constexpr bool value = __is_empty(T) && !__is_final(T);
#else
        static constexpr bool value = false;
#endif
};

struct empty_init_t {};

namespace empty_impl {

template <typename T, unsigned N = 0, 
          bool E = boost::multiprecision::detail::use_empty_value_base<T>::value>
class empty_value
{
private:
    T value_;

public:
    using type = T;

    empty_value() = default;
    explicit empty_value(boost::multiprecision::detail::empty_init_t) : value_ {} {}

    template <typename U, typename... Args>
    empty_value(boost::multiprecision::detail::empty_init_t, U&& value, Args&&... args) :
        value_ {std::forward<U>(value), std::forward<Args>(args)...} {}

    const T& get() const noexcept { return value_; }
    T& get() noexcept { return value_; }
};

template <typename T, unsigned N>
class empty_value<T, N, true> : T
{
public:
    using type = T;

    empty_value() = default;
    explicit empty_value(boost::multiprecision::detail::empty_init_t) : T{} {}

    template <typename U, typename... Args>
    empty_value(boost::multiprecision::detail::empty_init_t, U&& value, Args&&... args) :
        T{std::forward<U>(value), std::forward<Args>(args)...} {}

    const T& get() const noexcept { return *this; }
    T& get() noexcept { return *this; }
};

} // Namespace empty impl

using empty_impl::empty_value;

BOOST_INLINE_CONSTEXPR empty_init_t empty_init = empty_init_t();

}}} // Namespaces

#endif // BOOST_MP_DETAIL_EMPTY_VALUE_HPP
