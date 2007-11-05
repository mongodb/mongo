/* boost random/tausworthe.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: linear_feedback_shift.hpp,v 1.12 2005/05/21 15:57:00 dgregor Exp $
 *
 */

#ifndef BOOST_RANDOM_LINEAR_FEEDBACK_SHIFT_HPP
#define BOOST_RANDOM_LINEAR_FEEDBACK_SHIFT_HPP

#include <iostream>
#include <cassert>
#include <stdexcept>
#include <boost/config.hpp>
#include <boost/static_assert.hpp>
#include <boost/limits.hpp>

namespace boost {
namespace random {

// Tausworte 1965
template<class UIntType, int w, int k, int q, int s, UIntType val>
class linear_feedback_shift
{
public:
  typedef UIntType result_type;
  // avoid the warning trouble when using (1<<w) on 32 bit machines
  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);
  BOOST_STATIC_CONSTANT(int, word_size = w);
  BOOST_STATIC_CONSTANT(int, exponent1 = k);
  BOOST_STATIC_CONSTANT(int, exponent2 = q);
  BOOST_STATIC_CONSTANT(int, step_size = s);

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return 0; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return wordmask; }

  // MSVC 6 and possibly others crash when encountering complicated integral
  // constant expressions.  Avoid the checks for now.
  // BOOST_STATIC_ASSERT(w > 0);
  // BOOST_STATIC_ASSERT(q > 0);
  // BOOST_STATIC_ASSERT(k < w);
  // BOOST_STATIC_ASSERT(0 < 2*q && 2*q < k);
  // BOOST_STATIC_ASSERT(0 < s && s <= k-q);

  explicit linear_feedback_shift(UIntType s0 = 341) : wordmask(0)
  {
    // MSVC fails BOOST_STATIC_ASSERT with std::numeric_limits at class scope
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(std::numeric_limits<UIntType>::is_integer);
    BOOST_STATIC_ASSERT(!std::numeric_limits<UIntType>::is_signed);
#endif

    // avoid "left shift count >= with of type" warning
    for(int i = 0; i < w; ++i)
      wordmask |= (1u << i);
    seed(s0);
  }

  template<class It> linear_feedback_shift(It& first, It last) : wordmask(0)
  {
    // MSVC fails BOOST_STATIC_ASSERT with std::numeric_limits at class scope
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(std::numeric_limits<UIntType>::is_integer);
    BOOST_STATIC_ASSERT(!std::numeric_limits<UIntType>::is_signed);
#endif

    // avoid "left shift count >= with of type" warning
    for(int i = 0; i < w; ++i)
      wordmask |= (1u << i);
    seed(first, last);
  }

  void seed(UIntType s0 = 341) { assert(s0 >= (1 << (w-k))); value = s0; }
  template<class It> void seed(It& first, It last)
  {
    if(first == last)
      throw std::invalid_argument("linear_feedback_shift::seed");
    value = *first++;
    assert(value >= (1 << (w-k)));
  }

  result_type operator()()
  {
    const UIntType b = (((value << q) ^ value) & wordmask) >> (k-s);
    const UIntType mask = ( (~static_cast<UIntType>(0)) << (w-k) ) & wordmask;
    value = ((value & mask) << s) ^ b;
    return value;
  }
  bool validation(result_type x) const { return val == x; }

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, linear_feedback_shift x)
  { os << x.value; return os; }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, linear_feedback_shift& x)
  { is >> x.value; return is; }
#endif

  friend bool operator==(linear_feedback_shift x, linear_feedback_shift y)
  { return x.value == y.value; }
  friend bool operator!=(linear_feedback_shift x, linear_feedback_shift y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(linear_feedback_shift rhs) const
  { return value == rhs.value; }
  bool operator!=(linear_feedback_shift rhs) const
  { return !(*this == rhs); }
#endif

private:
  UIntType wordmask; // avoid "left shift count >= width of type" warnings
  UIntType value;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class UIntType, int w, int k, int q, int s, UIntType val>
const bool linear_feedback_shift<UIntType, w, k, q, s, val>::has_fixed_range;
template<class UIntType, int w, int k, int q, int s, UIntType val>
const int linear_feedback_shift<UIntType, w, k, q, s, val>::word_size;
template<class UIntType, int w, int k, int q, int s, UIntType val>
const int linear_feedback_shift<UIntType, w, k, q, s, val>::exponent1;
template<class UIntType, int w, int k, int q, int s, UIntType val>
const int linear_feedback_shift<UIntType, w, k, q, s, val>::exponent2;
template<class UIntType, int w, int k, int q, int s, UIntType val>
const int linear_feedback_shift<UIntType, w, k, q, s, val>::step_size;
#endif

} // namespace random
} // namespace boost

#endif // BOOST_RANDOM_LINEAR_FEEDBACK_SHIFT_HPP
