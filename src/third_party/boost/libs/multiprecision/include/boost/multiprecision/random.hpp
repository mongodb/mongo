///////////////////////////////////////////////////////////////
//  Copyright Jens Maurer 2006-2011
//  Copyright Steven Watanabe 2011
//  Copyright 2012 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_RANDOM_HPP
#define BOOST_MP_RANDOM_HPP

#include <boost/multiprecision/detail/standalone_config.hpp>

#if defined(__GNUC__) || defined(_MSC_VER)
#pragma message("NOTE: Use of this header (boost/multiprecision/random.hpp) is deprecated: please use the random number library headers directly.")
#endif

#ifndef BOOST_MP_STANDALONE
#include <boost/random.hpp>
#else
#error "Use of this header is removed in standalone mode"
#endif

#endif
