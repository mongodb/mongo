//  (C) Copyright Matt Borland 2021.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// We deliberately use assert in here:
//
// boost-no-inspect

#ifndef BOOST_MP_DETAIL_ASSERT_HPP
#define BOOST_MP_DETAIL_ASSERT_HPP

#include <boost/multiprecision/detail/standalone_config.hpp>

#ifndef BOOST_MP_STANDALONE

#include <boost/assert.hpp>
#define BOOST_MP_ASSERT(expr) BOOST_ASSERT(expr)
#define BOOST_MP_ASSERT_MSG(expr, msg) BOOST_ASSERT_MSG(expr, msg)

#else // Standalone mode - use cassert

#include <cassert>
#define BOOST_MP_ASSERT(expr) assert(expr)
#define BOOST_MP_ASSERT_MSG(expr, msg) assert((expr)&&(msg))

#endif

#endif // BOOST_MP_DETAIL_ASSERT_HPP
