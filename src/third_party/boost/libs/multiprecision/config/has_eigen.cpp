//  Copyright John Maddock 2011.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <Eigen/Dense>

int main()
{
#if EIGEN_VERSION_AT_LEAST(3, 3, 0)
#else
#error "Obsolete Eigen"
#endif
   return 0;
}
