/*
Copyright 2024 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_POINTER_IN_RANGE_HPP
#define BOOST_CORE_POINTER_IN_RANGE_HPP

#include <boost/config.hpp>
#include <functional>

#if !defined(BOOST_NO_CXX14_CONSTEXPR)
#if defined(BOOST_MSVC) && BOOST_MSVC >= 1925
#define BOOST_CORE_DETAIL_HAS_IS_CONSTEVAL
#elif defined(__has_builtin)
#if __has_builtin(__builtin_is_constant_evaluated)
#define BOOST_CORE_DETAIL_HAS_IS_CONSTEVAL
#endif
#endif
#endif

#if !defined(BOOST_CORE_DETAIL_HAS_IS_CONSTEVAL)
#define BOOST_CORE_NO_CONSTEXPR_POINTER_IN_RANGE
#endif

namespace boost {

template<class T>
inline BOOST_CONSTEXPR bool
pointer_in_range(const T* p, const T* b, const T* e)
{
#if defined(BOOST_CORE_DETAIL_HAS_IS_CONSTEVAL)
    if ( __builtin_is_constant_evaluated()) {
        for (; b != e; ++b) {
            if (b == p) {
                return true;
            }
        }
        return false;
    }
#endif
    return std::less_equal<const T*>()(b, p) && std::less<const T*>()(p, e);
}

} /* boost */

#endif
