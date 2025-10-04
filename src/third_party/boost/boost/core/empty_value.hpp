/*
Copyright 2018 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_EMPTY_VALUE_HPP
#define BOOST_CORE_EMPTY_VALUE_HPP

#include <boost/config.hpp>
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#include <utility>
#endif

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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4510)
#endif

namespace boost {

template<class T>
struct use_empty_value_base {
    enum {
#if defined(BOOST_DETAIL_EMPTY_VALUE_BASE)
        value = __is_empty(T) && !__is_final(T)
#else
        value = false
#endif
    };
};

struct empty_init_t { };

namespace empty_ {

template<class T, unsigned N = 0,
    bool E = boost::use_empty_value_base<T>::value>
class empty_value {
public:
    typedef T type;

#if !defined(BOOST_NO_CXX11_DEFAULTED_FUNCTIONS)
    empty_value() = default;
#else
    BOOST_CONSTEXPR empty_value() { }
#endif

    BOOST_CONSTEXPR empty_value(boost::empty_init_t)
        : value_() { }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
    template<class U, class... Args>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U&& value, Args&&... args)
        : value_(std::forward<U>(value), std::forward<Args>(args)...) { }
#else
    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U&& value)
        : value_(std::forward<U>(value)) { }
#endif
#else
    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, const U& value)
        : value_(value) { }

    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U& value)
        : value_(value) { }
#endif

    BOOST_CONSTEXPR const T& get() const BOOST_NOEXCEPT {
        return value_;
    }

    BOOST_CXX14_CONSTEXPR T& get() BOOST_NOEXCEPT {
        return value_;
    }

private:
    T value_;
};

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
#if defined(BOOST_MSVC)
/*
This is a workaround to an MSVC bug when T is a nested class:
https://developercommunity.visualstudio.com/t/Compiler-bug:-Incorrect-C2247-and-C2248/10690025
*/
namespace detail {

template<class T>
class empty_value_base
    : public T {
public:
#if !defined(BOOST_NO_CXX11_DEFAULTED_FUNCTIONS)
    empty_value_base() = default;
#else
    BOOST_CONSTEXPR empty_value_base() { }
#endif

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
    template<class U, class... Args>
    BOOST_CONSTEXPR empty_value_base(U&& value, Args&&... args)
        : T(std::forward<U>(value), std::forward<Args>(args)...) { }
#else
    template<class U>
    BOOST_CONSTEXPR empty_value_base(U&& value)
        : T(std::forward<U>(value)) { }
#endif
#else
    template<class U>
    BOOST_CONSTEXPR empty_value_base(const U& value)
        : T(value) { }

    template<class U>
    BOOST_CONSTEXPR empty_value_base(U& value)
        : T(value) { }
#endif
};

} /* detail */
#endif

template<class T, unsigned N>
class empty_value<T, N, true>
#if defined(BOOST_MSVC)
    : detail::empty_value_base<T> {
    typedef detail::empty_value_base<T> empty_base_;
#else
    : T {
    typedef T empty_base_;
#endif

public:
    typedef T type;

#if !defined(BOOST_NO_CXX11_DEFAULTED_FUNCTIONS)
    empty_value() = default;
#else
    BOOST_CONSTEXPR empty_value() { }
#endif

    BOOST_CONSTEXPR empty_value(boost::empty_init_t)
        : empty_base_() { }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
    template<class U, class... Args>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U&& value, Args&&... args)
        : empty_base_(std::forward<U>(value), std::forward<Args>(args)...) { }
#else
    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U&& value)
        : empty_base_(std::forward<U>(value)) { }
#endif
#else
    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, const U& value)
        : empty_base_(value) { }

    template<class U>
    BOOST_CONSTEXPR empty_value(boost::empty_init_t, U& value)
        : empty_base_(value) { }
#endif

    BOOST_CONSTEXPR const T& get() const BOOST_NOEXCEPT {
        return *this;
    }

    BOOST_CXX14_CONSTEXPR T& get() BOOST_NOEXCEPT {
        return *this;
    }
};
#endif

} /* empty_ */

using empty_::empty_value;

BOOST_INLINE_CONSTEXPR empty_init_t empty_init = empty_init_t();

} /* boost */

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif
