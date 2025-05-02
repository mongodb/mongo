/*
Copyright 2023 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_DATA_HPP
#define BOOST_CORE_DATA_HPP

#include <initializer_list>
#include <cstddef>

namespace boost {

template<class C>
inline constexpr auto
data(C& c) noexcept(noexcept(c.data())) -> decltype(c.data())
{
    return c.data();
}

template<class C>
inline constexpr auto
data(const C& c) noexcept(noexcept(c.data())) -> decltype(c.data())
{
    return c.data();
}

template<class T, std::size_t N>
inline constexpr T*
data(T(&a)[N]) noexcept
{
    return a;
}

template<class T>
inline constexpr const T*
data(std::initializer_list<T> l) noexcept
{
    return l.begin();
}

} /* boost */

#endif
