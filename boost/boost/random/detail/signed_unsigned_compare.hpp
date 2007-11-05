/* boost random/detail/signed_unsigned_compare.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * Revision history
 */


#ifndef BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_COMPARE
#define BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_COMPARE

#include <boost/limits.hpp>

namespace boost {
namespace random {

/*
 * Correctly compare two numbers whose types possibly differ in signedness.
 * See boost::numeric_cast<> for the general idea.
 * Most "if" statements involve only compile-time constants, so the
 * optimizing compiler can do its job easily.
 *
 * With most compilers, the straightforward implementation produces a
 * bunch of (legitimate) warnings.  Some template magic helps, though.
 */

namespace detail {
template<bool signed1, bool signed2>
struct do_compare
{ };

template<>
struct do_compare<false, false>
{
  // cast to the larger type is automatic with built-in types
  template<class T1, class T2>
  static bool equal(T1 x, T2 y) { return x == y; }
  template<class T1, class T2>
  static bool lessthan(T1 x, T2 y) { return x < y; }
};

template<>
struct do_compare<true, true> : do_compare<false, false>
{ };

template<>
struct do_compare<true, false>
{
  template<class T1, class T2>
  static bool equal(T1 x, T2 y) { return x >= 0 && static_cast<T2>(x) == y; }
  template<class T1, class T2>
  static bool lessthan(T1 x, T2 y) { return x < 0 || static_cast<T2>(x) < y; }
};

template<>
struct do_compare<false, true>
{
  template<class T1, class T2>
  static bool equal(T1 x, T2 y) { return y >= 0 && x == static_cast<T1>(y); }
  template<class T1, class T2>
  static bool lessthan(T1 x, T2 y) { return y >= 0 && x < static_cast<T1>(y); }
};

} // namespace detail


template<class T1, class T2>
int equal_signed_unsigned(T1 x, T2 y)
{
  typedef std::numeric_limits<T1> x_traits;
  typedef std::numeric_limits<T2> y_traits;
  return detail::do_compare<x_traits::is_signed, y_traits::is_signed>::equal(x, y);
}

template<class T1, class T2>
int lessthan_signed_unsigned(T1 x, T2 y)
{
  typedef std::numeric_limits<T1> x_traits;
  typedef std::numeric_limits<T2> y_traits;
  return detail::do_compare<x_traits::is_signed, y_traits::is_signed>::lessthan(x, y);
}

} // namespace random
} // namespace boost

#endif // BOOST_RANDOM_DETAIL_SIGNED_UNSIGNED_COMPARE
