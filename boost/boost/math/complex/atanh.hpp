//  (C) Copyright John Maddock 2005.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_COMPLEX_ATANH_INCLUDED
#define BOOST_MATH_COMPLEX_ATANH_INCLUDED

#ifndef BOOST_MATH_COMPLEX_DETAILS_INCLUDED
#  include <boost/math/complex/details.hpp>
#endif
#ifndef BOOST_MATH_LOG1P_INCLUDED
#  include <boost/math/special_functions/log1p.hpp>
#endif
#include <boost/assert.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{ using ::sqrt; using ::fabs; using ::acos; using ::asin; using ::atan; using ::atan2; }
#endif

namespace boost{ namespace math{

template<class T> 
std::complex<T> atanh(const std::complex<T>& z)
{
   //
   // References:
   //
   // Eric W. Weisstein. "Inverse Hyperbolic Tangent." 
   // From MathWorld--A Wolfram Web Resource. 
   // http://mathworld.wolfram.com/InverseHyperbolicTangent.html
   //
   // Also: The Wolfram Functions Site,
   // http://functions.wolfram.com/ElementaryFunctions/ArcTanh/
   //
   // Also "Abramowitz and Stegun. Handbook of Mathematical Functions."
   // at : http://jove.prohosting.com/~skripty/toc.htm
   //
   
   static const T half_pi = static_cast<T>(1.57079632679489661923132169163975144L);
   static const T pi = static_cast<T>(3.141592653589793238462643383279502884197L);
   static const T one = static_cast<T>(1.0L);
   static const T two = static_cast<T>(2.0L);
   static const T four = static_cast<T>(4.0L);
   static const T zero = static_cast<T>(0);
   static const T a_crossover = static_cast<T>(0.3L);

   T x = std::fabs(z.real());
   T y = std::fabs(z.imag());

   T real, imag;  // our results

   T safe_upper = detail::safe_max(two);
   T safe_lower = detail::safe_min(static_cast<T>(2));

   //
   // Begin by handling the special cases specified in C99:
   //
   if(detail::test_is_nan(x))
   {
      if(detail::test_is_nan(y))
         return std::complex<T>(x, x);
      else if(std::numeric_limits<T>::has_infinity && (y == std::numeric_limits<T>::infinity()))
         return std::complex<T>(0, ((z.imag() < 0) ? -half_pi : half_pi));
      else
         return std::complex<T>(x, x);
   }
   else if(detail::test_is_nan(y))
   {
      if(x == 0)
         return std::complex<T>(x, y);
      if(std::numeric_limits<T>::has_infinity && (x == std::numeric_limits<T>::infinity()))
         return std::complex<T>(0, y);
      else
         return std::complex<T>(y, y);
   }
   else if((x > safe_lower) && (x < safe_upper) && (y > safe_lower) && (y < safe_upper))
   {

      T xx = x*x;
      T yy = y*y;
      T x2 = x * two;

      ///
      // The real part is given by:
      // 
      // real(atanh(z)) == log((1 + x^2 + y^2 + 2x) / (1 + x^2 + y^2 - 2x))
      // 
      // However, when x is either large (x > 1/E) or very small
      // (x < E) then this effectively simplifies
      // to log(1), leading to wildly inaccurate results.  
      // By dividing the above (top and bottom) by (1 + x^2 + y^2) we get:
      //
      // real(atanh(z)) == log((1 + (2x / (1 + x^2 + y^2))) / (1 - (-2x / (1 + x^2 + y^2))))
      //
      // which is much more sensitive to the value of x, when x is not near 1
      // (remember we can compute log(1+x) for small x very accurately).
      //
      // The cross-over from one method to the other has to be determined
      // experimentally, the value used below appears correct to within a 
      // factor of 2 (and there are larger errors from other parts
      // of the input domain anyway).
      //
      T alpha = two*x / (one + xx + yy);
      if(alpha < a_crossover)
      {
         real = boost::math::log1p(alpha) - boost::math::log1p(-alpha);
      }
      else
      {
         T xm1 = x - one;
         real = boost::math::log1p(x2 + xx + yy) - std::log(xm1*xm1 + yy);
      }
      real /= four;
      if(z.real() < 0)
         real = -real;

      imag = std::atan2((y * two), (one - xx - yy));
      imag /= two;
      if(z.imag() < 0)
         imag = -imag;
   }
   else
   {
      //
      // This section handles exception cases that would normally cause
      // underflow or overflow in the main formulas.
      //
      // Begin by working out the real part, we need to approximate
      //    alpha = 2x / (1 + x^2 + y^2)
      // without either overflow or underflow in the squared terms.
      //
      T alpha = 0;
      if(x >= safe_upper)
      {
         // this is really a test for infinity, 
         // but we may not have the necessary numeric_limits support:
         if((x > (std::numeric_limits<T>::max)()) || (y > (std::numeric_limits<T>::max)()))
         {
            alpha = 0;
         }
         else if(y >= safe_upper)
         {
            // Big x and y: divide alpha through by x*y:
            alpha = (two/y) / (x/y + y/x);
         }
         else if(y > one)
         {
            // Big x: divide through by x:
            alpha = two / (x + y*y/x);
         }
         else
         {
            // Big x small y, as above but neglect y^2/x:
            alpha = two/x;
         }
      }
      else if(y >= safe_upper)
      {
         if(x > one)
         {
            // Big y, medium x, divide through by y:
            alpha = (two*x/y) / (y + x*x/y);
         }
         else
         {
            // Small x and y, whatever alpha is, it's too small to calculate:
            alpha = 0;
         }
      }
      else
      {
         // one or both of x and y are small, calculate divisor carefully:
         T div = one;
         if(x > safe_lower)
            div += x*x;
         if(y > safe_lower)
            div += y*y;
         alpha = two*x/div;
      }
      if(alpha < a_crossover)
      {
         real = boost::math::log1p(alpha) - boost::math::log1p(-alpha);
      }
      else
      {
         // We can only get here as a result of small y and medium sized x,
         // we can simply neglect the y^2 terms:
         BOOST_ASSERT(x >= safe_lower);
         BOOST_ASSERT(x <= safe_upper);
         //BOOST_ASSERT(y <= safe_lower);
         T xm1 = x - one;
         real = std::log(1 + two*x + x*x) - std::log(xm1*xm1);
      }
      
      real /= four;
      if(z.real() < 0)
         real = -real;

      //
      // Now handle imaginary part, this is much easier,
      // if x or y are large, then the formula:
      //    atan2(2y, 1 - x^2 - y^2)
      // evaluates to +-(PI - theta) where theta is negligible compared to PI.
      //
      if((x >= safe_upper) || (y >= safe_upper))
      {
         imag = pi;
      }
      else if(x <= safe_lower)
      {
         //
         // If both x and y are small then atan(2y),
         // otherwise just x^2 is negligible in the divisor:
         //
         if(y <= safe_lower)
            imag = std::atan2(two*y, one);
         else
         {
            if((y == zero) && (x == zero))
               imag = 0;
            else
               imag = std::atan2(two*y, one - y*y);
         }
      }
      else
      {
         //
         // y^2 is negligible:
         //
         if((y == zero) && (x == one))
            imag = 0;
         else
            imag = std::atan2(two*y, 1 - x*x);
      }
      imag /= two;
      if(z.imag() < 0)
         imag = -imag;
   }
   return std::complex<T>(real, imag);
}

} } // namespaces

#endif // BOOST_MATH_COMPLEX_ATANH_INCLUDED
