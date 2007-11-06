/* Boost interval/detail/bugs.hpp file
 *
 * Copyright 2000 Jens Maurer
 * Copyright 2002 Hervé Brönnimann, Guillaume Melquiond, Sylvain Pion
 *
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or
 * copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#ifndef BOOST_NUMERIC_INTERVAL_DETAIL_BUGS
#define BOOST_NUMERIC_INTERVAL_DETAIL_BUGS

#include <boost/config.hpp>

#if defined(__GLIBC__) && (defined(__USE_MISC) || defined(__USE_XOPEN_EXTENDED) || defined(__USE_ISOC99)) && !defined(__ICC)
#  define BOOST_HAS_INV_HYPERBOLIC
#endif

#ifdef BOOST_NO_STDC_NAMESPACE
#  define BOOST_NUMERIC_INTERVAL_using_math(a) using ::a
#  ifdef BOOST_HAS_INV_HYPERBOLIC
#    define BOOST_NUMERIC_INTERVAL_using_ahyp(a) using ::a
#  endif
#else
#  define BOOST_NUMERIC_INTERVAL_using_math(a) using std::a
#  if defined(BOOST_HAS_INV_HYPERBOLIC)
#    if defined(__GLIBCPP__) || defined(__GLIBCXX__)
#    define BOOST_NUMERIC_INTERVAL_using_ahyp(a) using ::a
#    else
#    define BOOST_NUMERIC_INTERVAL_using_ahyp(a) using std::a
#  endif
#  endif
#endif

#if defined(__COMO__) || defined(BOOST_INTEL)
#  define BOOST_NUMERIC_INTERVAL_using_max(a) using std::a
#elif defined(BOOST_NO_STDC_NAMESPACE)
#  define BOOST_NUMERIC_INTERVAL_using_max(a) using ::a
#else
#  define BOOST_NUMERIC_INTERVAL_using_max(a) using std::a
#endif

#ifndef BOOST_NUMERIC_INTERVAL_using_ahyp
#  define BOOST_NUMERIC_INTERVAL_using_ahyp(a)
#endif

#if defined(__GNUC__) && (__GNUC__ <= 2)
// cf PR c++/1981 for a description of the bug
#include <algorithm>
#include <cmath>
namespace boost {
namespace numeric {
  using std::min;
  using std::max;
  using std::sqrt;
  using std::exp;
  using std::log;
  using std::cos;
  using std::tan;
  using std::asin;
  using std::acos;
  using std::atan;
  using std::ceil;
  using std::floor;
  using std::sinh;
  using std::cosh;
  using std::tanh;
# undef BOOST_NUMERIC_INTERVAL_using_max
# undef BOOST_NUMERIC_INTERVAL_using_math
# define BOOST_NUMERIC_INTERVAL_using_max(a)
# define BOOST_NUMERIC_INTERVAL_using_math(a)
# undef BOOST_NUMERIC_INTERVAL_using_ahyp
# define BOOST_NUMERIC_INTERVAL_using_ahyp(a)
} // namespace numeric
} // namespace boost
#endif

#endif // BOOST_NUMERIC_INTERVAL_DETAIL_BUGS
