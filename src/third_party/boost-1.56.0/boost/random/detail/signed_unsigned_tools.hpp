/* boost random/detail/signed_unsigned_tools.hpp header file
 *
 * Copyright Jens Maurer 2006
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 */

#ifndef BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_TOOLS
#define BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_TOOLS

#include <boost/limits.hpp>
#include <boost/config.hpp>
#include <boost/type_traits/make_unsigned.hpp>

namespace boost {
namespace random {
namespace detail {


/*
 * Compute x - y, we know that x >= y, return an unsigned value.
 */

template<class T, bool sgn = std::numeric_limits<T>::is_signed>
struct subtract { };

template<class T>
struct subtract<T, /* signed */ false>
{
  typedef T result_type;
  result_type operator()(T x, T y) { return x - y; }
};

template<class T>
struct subtract<T, /* signed */ true>
{
  typedef typename make_unsigned<T>::type result_type;
  result_type operator()(T x, T y)
  {
    if (y >= 0)   // because x >= y, it follows that x >= 0, too
      return result_type(x) - result_type(y);
    if (x >= 0)   // y < 0
      // avoid the nasty two's complement case for y == min()
      return result_type(x) + result_type(-(y+1)) + 1;
    // both x and y are negative: no signed overflow
    return result_type(x - y);
  }
};

/*
 * Compute x + y, x is unsigned, result fits in type of "y".
 */

template<class T1, class T2, bool sgn = std::numeric_limits<T2>::is_signed>
struct add { };

template<class T1, class T2>
struct add<T1, T2, /* signed */ false>
{
  typedef T2 result_type;
  result_type operator()(T1 x, T2 y) { return T2(x) + y; }
};

template<class T1, class T2>
struct add<T1, T2, /* signed */ true>
{
  typedef T2 result_type;
  result_type operator()(T1 x, T2 y)
  {
    if (y >= 0)
      return T2(x) + y;
    // y < 0
    if (x > T1(-(y+1)))  // result >= 0 after subtraction
      // avoid the nasty two's complement edge case for y == min()
      return T2(x - T1(-(y+1)) - 1);
    // abs(x) < abs(y), thus T2 able to represent x
    return T2(x) + y;
  }
};

} // namespace detail
} // namespace random
} // namespace boost

#endif // BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_TOOLS

