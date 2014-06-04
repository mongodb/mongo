/*
 * Copyright (c) 2012 Glen Joseph Fernandes 
 * glenfe at live dot com
 *
 * Distributed under the Boost Software License, 
 * Version 1.0. (See accompanying file LICENSE_1_0.txt 
 * or copy at http://boost.org/LICENSE_1_0.txt)
 */
#ifndef BOOST_SMART_PTR_DETAIL_ARRAY_TRAITS_HPP
#define BOOST_SMART_PTR_DETAIL_ARRAY_TRAITS_HPP

#include <boost/type_traits/remove_cv.hpp>

namespace boost {
    namespace detail {
        template<typename T>
        struct array_base {
            typedef typename boost::remove_cv<T>::type type;
        };
        template<typename T>
        struct array_base<T[]> {
            typedef typename array_base<T>::type type;
        };
        template<typename T, std::size_t N>
        struct array_base<T[N]> {
            typedef typename array_base<T>::type type;
        };
        template<typename T>
        struct array_total {
            enum {
                size = 1
            };
        };
        template<typename T, std::size_t N>
        struct array_total<T[N]> {
            enum {
                size = N * array_total<T>::size
            };
        };
        template<typename T> 
        struct array_inner;
        template<typename T>
        struct array_inner<T[]> {
            typedef T type;
        };
        template<typename T, std::size_t N>
        struct array_inner<T[N]> {
            typedef T type;
        };
    }
}

#endif
