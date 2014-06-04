#ifndef BOOST_ATOMIC_DETAIL_TYPE_CLASSIFICATION_HPP
#define BOOST_ATOMIC_DETAIL_TYPE_CLASSIFICATION_HPP

//  Copyright (c) 2011 Helge Bahmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#include <boost/atomic/detail/config.hpp>
#include <boost/type_traits/is_integral.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

template<typename T, bool IsInt = boost::is_integral<T>::value>
struct classify
{
    typedef void type;
};

template<typename T>
struct classify<T, true> {typedef int type;};

template<typename T>
struct classify<T*, false> {typedef void* type;};

template<typename T>
struct storage_size_of
{
    enum _
    {
        size = sizeof(T),
        value = (size == 3 ? 4 : (size >= 5 && size <= 7 ? 8 : (size >= 9 && size <= 15 ? 16 : size)))
    };
};

}}}

#endif
