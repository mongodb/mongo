
// Copyright (C) 2008-2011 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_FWD_HPP_INCLUDED
#define BOOST_UNORDERED_FWD_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <memory>
#include <functional>
#include <boost/functional/hash_fwd.hpp>

namespace boost
{
namespace unordered
{
    template <class K,
        class T,
        class H = boost::hash<K>,
        class P = std::equal_to<K>,
        class A = std::allocator<std::pair<const K, T> > >
    class unordered_map;

    template <class K,
        class T,
        class H = boost::hash<K>,
        class P = std::equal_to<K>,
        class A = std::allocator<std::pair<const K, T> > >
    class unordered_multimap;

    template <class T,
        class H = boost::hash<T>,
        class P = std::equal_to<T>,
        class A = std::allocator<T> >
    class unordered_set;

    template <class T,
        class H = boost::hash<T>,
        class P = std::equal_to<T>,
        class A = std::allocator<T> >
    class unordered_multiset;

    struct piecewise_construct_t {};
    const piecewise_construct_t piecewise_construct = piecewise_construct_t();
}
}

#endif
