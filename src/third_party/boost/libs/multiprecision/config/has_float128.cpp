//  Copyright John Maddock 2013.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/config.hpp>

#ifndef BOOST_HAS_FLOAT128
#error "This doesn't work unless Boost.Config enables __float128 support"
#endif

extern "C" {
#include <quadmath.h>
}

int main()
{
   __float128 f = -2.0Q;
   f            = fabsq(f);
   f            = expq(f);

   return 0;
}
