//  (C) Copyright John Maddock 2005.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_EXPM1_INCLUDED
#define BOOST_MATH_EXPM1_INCLUDED

#include <cmath>
#include <math.h> // platform's ::expm1
#include <boost/limits.hpp>
#include <boost/math/special_functions/detail/series.hpp>

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
#  include <boost/static_assert.hpp>
#else
#  include <boost/assert.hpp>
#endif

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{ using ::exp; using ::fabs; }
#endif


namespace boost{ namespace math{

namespace detail{
//
// Functor expm1_series returns the next term in the Taylor series
// x^k / k!
// each time that operator() is invoked.
//
template <class T>
struct expm1_series
{
   typedef T result_type;

   expm1_series(T x)
      : k(0), m_x(x), m_term(1) {}

   T operator()()
   {
      ++k;
      m_term *= m_x;
      m_term /= k;
      return m_term; 
   }

   int count()const
   {
      return k;
   }

private:
   int k;
   const T m_x;
   T m_term;
   expm1_series(const expm1_series&);
   expm1_series& operator=(const expm1_series&);
};

} // namespace

//
// Algorithm expm1 is part of C99, but is not yet provided by many compilers.
//
// This version uses a Taylor series expansion for 0.5 > |x| > epsilon.
//
template <class T>
T expm1(T x)
{
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
   BOOST_STATIC_ASSERT(::std::numeric_limits<T>::is_specialized);
#else
   BOOST_ASSERT(std::numeric_limits<T>::is_specialized);
#endif

   T a = std::fabs(x);
   if(a > T(0.5L))
      return std::exp(x) - T(1);
   if(a < std::numeric_limits<T>::epsilon())
      return x;
   detail::expm1_series<T> s(x);
   T result = detail::kahan_sum_series(s, std::numeric_limits<T>::digits + 2);
   return result;
}
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
inline float expm1(float z)
{
   return expm1<float>(z);
}
inline double expm1(double z)
{
   return expm1<double>(z);
}
inline long double expm1(long double z)
{
   return expm1<long double>(z);
}
#endif

#ifdef expm1
#  ifndef BOOST_HAS_expm1
#     define BOOST_HAS_expm1
#  endif
#  undef expm1
#endif

#ifdef BOOST_HAS_EXPM1
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
inline float expm1(float x){ return ::expm1f(x); }
inline long double expm1(long double x){ return ::expm1l(x); }
#else
inline float expm1(float x){ return ::expm1(x); }
#endif
inline double expm1(double x){ return ::expm1(x); }
#endif

} } // namespaces

#endif // BOOST_MATH_HYPOT_INCLUDED
