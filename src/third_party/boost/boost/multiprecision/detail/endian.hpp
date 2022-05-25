////////////////////////////////////////////////////////////////
//  Copyright 2021 Matt Borland. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_ENDIAN_HPP
#define BOOST_MP_ENDIAN_HPP

#include <boost/multiprecision/detail/standalone_config.hpp>

#ifndef BOOST_MP_STANDALONE

#  include <boost/predef/other/endian.h>
#  define BOOST_MP_ENDIAN_BIG_BYTE BOOST_ENDIAN_BIG_BYTE
#  define BOOST_MP_ENDIAN_LITTLE_BYTE BOOST_ENDIAN_LITTLE_BYTE

#elif defined(_WIN32)

#  define BOOST_MP_ENDIAN_BIG_BYTE 0
#  define BOOST_MP_ENDIAN_LITTLE_BYTE 1

#elif defined(__BYTE_ORDER__)

#  define BOOST_MP_ENDIAN_BIG_BYTE (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  define BOOST_MP_ENDIAN_LITTLE_BYTE (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

#else
#  error Could not determine endian type. Please disable standalone mode, and file an issue at https://github.com/boostorg/multiprecision
#endif // Determine endianness

static_assert((BOOST_MP_ENDIAN_BIG_BYTE || BOOST_MP_ENDIAN_LITTLE_BYTE)
    && !(BOOST_MP_ENDIAN_BIG_BYTE && BOOST_MP_ENDIAN_LITTLE_BYTE),
    "Inconsistent endianness detected. Please disable standalone mode, and file an issue at https://github.com/boostorg/multiprecision");

#endif // BOOST_MP_ENDIAN_HPP
