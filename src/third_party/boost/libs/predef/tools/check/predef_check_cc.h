/*
Copyright Rene Rivera 2015
Distributed under the Boost Software License, Version 1.0.
(See accompanying file LICENSE_1_0.txt or copy at
http://www.boost.org/LICENSE_1_0.txt)
*/
#include <boost/predef.h>

#ifdef CHECK
#   if ((CHECK) == 0)
#       error "FAILED"
#   endif
#endif

int dummy()
{
	static int d = 0;
	return d++;
}
