
// Copyright 2005-2009 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// A non-portable hash function form non-zero floats on x86.
//
// Even if you're on an x86 platform, this might not work if their floating
// point isn't set up as this expects. So this should only be used if it's
// absolutely certain that it will work.

#if !defined(BOOST_FUNCTIONAL_HASH_DETAIL_HASH_FLOAT_X86_HEADER)
#define BOOST_FUNCTIONAL_HASH_DETAIL_HASH_FLOAT_X86_HEADER

#include <boost/cstdint.hpp>

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

namespace boost
{
    namespace hash_detail
    {
        inline void hash_float_combine(std::size_t& seed, std::size_t value)
        {
            seed ^= value + (seed<<6) + (seed>>2);
        }

        inline std::size_t float_hash_impl(float v)
        {
            boost::uint32_t* ptr = (boost::uint32_t*)&v;
            std::size_t seed = *ptr;
            return seed;
        }

        inline std::size_t float_hash_impl(double v)
        {
            boost::uint32_t* ptr = (boost::uint32_t*)&v;
            std::size_t seed = *ptr++;
            hash_float_combine(seed, *ptr);
            return seed;
        }

        inline std::size_t float_hash_impl(long double v)
        {
            boost::uint32_t* ptr = (boost::uint32_t*)&v;
            std::size_t seed = *ptr++;
            hash_float_combine(seed, *ptr++);
            hash_float_combine(seed, *(boost::uint16_t*)ptr);
            return seed;
        }
    }
}

#endif
