/////////////////////////////////////////////////////////////////////////////
//
// Copyright 2005-2014 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  Based on Peter Dimov's proposal
//  http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2005/n1756.pdf
//  issue 6.18.
//
//  This also contains public domain code from MurmurHash. From the
//  MurmurHash header:
//
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.
//
// Copyright 2021 Ion Gaztanaga
// Refactored the original Boost ContainerHash library to avoid
// any heavy std header dependencies to just combine two hash
// values represented in a std::size_t type.
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////


#ifndef BOOST_INTRUSIVE_DETAIL_HASH_COMBINE_HPP
#define BOOST_INTRUSIVE_DETAIL_HASH_COMBINE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/cstdint.hpp>
#include "hash_mix.hpp"

namespace boost {
namespace intrusive {
namespace detail {

inline void hash_combine_size_t(std::size_t& seed, std::size_t value)
{
   seed = boost::intrusive::detail::hash_mix(seed + 0x9e3779b9 + value);
}

}  //namespace detail {
}  //namespace intrusive {
}  //namespace boost {

#endif   //BOOST_INTRUSIVE_DETAIL_HASH_COMBINE_HPP
