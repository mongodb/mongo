//  Copyright John Maddock 2011.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <tommath.h>

int main()
{
   mp_int v;
   mp_init(&v);
   if (v.dp)
      mp_clear(&v);
   return 0;
}
