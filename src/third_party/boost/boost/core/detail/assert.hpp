/*
Copyright 2025 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#undef BOOST_CORE_DETAIL_ASSERT

#if !defined(__clang__) && \
    !defined(__INTEL_COMPILER) && \
    defined(__GNUC__) && \
    (__GNUC__ < 5)
#define BOOST_CORE_DETAIL_ASSERT(expr) void(0)
#else
#include <boost/assert.hpp>
#define BOOST_CORE_DETAIL_ASSERT(expr) BOOST_ASSERT(expr)
#endif
