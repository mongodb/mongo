//  (C) Copyright Matt Borland 2021 - 2023.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// We deliberately use assert in here:
//
// boost-no-inspect

#ifndef BOOST_NUMERIC_ODEINT_TOOLS_ASSERT_HPP
#define BOOST_NUMERIC_ODEINT_TOOLS_ASSERT_HPP

#include <boost/numeric/odeint/tools/is_standalone.hpp>

#ifndef BOOST_NUMERIC_ODEINT_STANDALONE

#include <boost/assert.hpp>

#define BOOST_NUMERIC_ODEINT_ASSERT(expr) BOOST_ASSERT(expr)
#define BOOST_NUMERIC_ODEINT_ASSERT_MSG(expr, msg) BOOST_ASSERT_MSG(expr, msg)

#else // Standalone mode so we use cassert

#include <cassert>
#define BOOST_NUMERIC_ODEINT_ASSERT(expr) assert(expr)
#define BOOST_NUMERIC_ODEINT_ASSERT_MSG(expr, msg) assert((expr)&&(msg))

#endif

#endif //BOOST_NUMERIC_ODEINT_TOOLS_ASSERT_HPP
