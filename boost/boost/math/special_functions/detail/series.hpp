//  (C) Copyright John Maddock 2005.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_SERIES_INCLUDED
#define BOOST_MATH_SERIES_INCLUDED

#include <cmath>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{ using ::pow; using ::fabs; }
#endif


namespace boost{ namespace math{ namespace detail{

//
// Algorithm kahan_sum_series invokes Functor func until the N'th
// term is too small to have any effect on the total, the terms 
// are added using the Kahan summation method.
//
// CAUTION: Optimizing compilers combined with extended-precision
// machine registers conspire to render this algorithm partly broken:
// double rounding of intermediate terms (first to a long double machine
// register, and then to a double result) cause the rounding error computed
// by the algorithm to be off by up to 1ulp.  However this occurs rarely, and 
// in any case the result is still much better than a naive summation.
//
template <class Functor>
typename Functor::result_type kahan_sum_series(Functor& func, int bits)
{
   typedef typename Functor::result_type result_type;
   result_type factor = std::pow(result_type(2), bits);
   result_type result = func();
   result_type next_term, y, t;
   result_type carry = 0;
   do{
      next_term = func();
      y = next_term - carry;
      t = result + y;
      carry = t - result;
      carry -= y;
      result = t;
   }
   while(std::fabs(result) < std::fabs(factor * next_term));
   return result;
}

} } } // namespaces

#endif // BOOST_MATH_SERIES_INCLUDED
