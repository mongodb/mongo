#ifndef BOOST_CORE_MAX_ALIGN_HPP_INCLUDED
#define BOOST_CORE_MAX_ALIGN_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  Copyright 2023 Peter Dimov
//  Distributed under the Boost Software License, Version 1.0.
//  https://www.boost.org/LICENSE_1_0.txt

#include <boost/core/alignof.hpp>
#include <boost/config.hpp>
#include <cstddef>

// BOOST_CORE_HAS_FLOAT128

#if defined(BOOST_HAS_FLOAT128)

# define BOOST_CORE_HAS_FLOAT128

#elif defined(__SIZEOF_FLOAT128__)

# define BOOST_CORE_HAS_FLOAT128

#elif defined(__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__ >= 404) && defined(__i386__)

# define BOOST_CORE_HAS_FLOAT128

#endif

// max_align_t, max_align

namespace boost
{
namespace core
{

union max_align_t
{
    char c;
    short s;
    int i;
    long l;

#if !defined(BOOST_NO_LONG_LONG)

    boost::long_long_type ll;

#endif

#if defined(BOOST_HAS_INT128)

    boost::int128_type i128;

#endif

    float f;
    double d;
    long double ld;

#if defined(BOOST_CORE_HAS_FLOAT128)

    __float128 f128;

#endif

    void* p;
    void (*pf) ();

    int max_align_t::* pm;
    void (max_align_t::*pmf)();
};

BOOST_CONSTEXPR_OR_CONST std::size_t max_align = BOOST_CORE_ALIGNOF( max_align_t );

} // namespace core
} // namespace boost

#endif  // #ifndef BOOST_CORE_MAX_ALIGN_HPP_INCLUDED
