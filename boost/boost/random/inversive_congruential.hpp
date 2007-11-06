/* boost random/inversive_congruential.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: inversive_congruential.hpp,v 1.11 2005/05/21 15:57:00 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_INVERSIVE_CONGRUENTIAL_HPP
#define BOOST_RANDOM_INVERSIVE_CONGRUENTIAL_HPP

#include <iostream>
#include <cassert>
#include <boost/config.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/detail/const_mod.hpp>

namespace boost {
namespace random {

// Eichenauer and Lehn 1986
template<class IntType, IntType a, IntType b, IntType p, IntType val>
class inversive_congruential
{
public:
  typedef IntType result_type;
#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
  static const bool has_fixed_range = true;
  static const result_type min_value = (b == 0 ? 1 : 0);
  static const result_type max_value = p-1;
#else
  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);
#endif
  BOOST_STATIC_CONSTANT(result_type, multiplier = a);
  BOOST_STATIC_CONSTANT(result_type, increment = b);
  BOOST_STATIC_CONSTANT(result_type, modulus = p);

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return b == 0 ? 1 : 0; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return p-1; }

  explicit inversive_congruential(IntType y0 = 1) : value(y0)
  {
    BOOST_STATIC_ASSERT(b >= 0);
    BOOST_STATIC_ASSERT(p > 1);
    BOOST_STATIC_ASSERT(a >= 1);
    if(b == 0) 
      assert(y0 > 0); 
  }
  template<class It> inversive_congruential(It& first, It last)
  { seed(first, last); }

  void seed(IntType y0 = 1) { value = y0; if(b == 0) assert(y0 > 0); }
  template<class It> void seed(It& first, It last)
  {
    if(first == last)
      throw std::invalid_argument("inversive_congruential::seed");
    value = *first++;
  }
  IntType operator()()
  {
    typedef const_mod<IntType, p> do_mod;
    value = do_mod::mult_add(a, do_mod::invert(value), b);
    return value;
  }

  bool validation(result_type x) const { return val == x; }

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, inversive_congruential x)
  { os << x.value; return os; }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, inversive_congruential& x)
  { is >> x.value; return is; }
#endif

  friend bool operator==(inversive_congruential x, inversive_congruential y)
  { return x.value == y.value; }
  friend bool operator!=(inversive_congruential x, inversive_congruential y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(inversive_congruential rhs) const
  { return value == rhs.value; }
  bool operator!=(inversive_congruential rhs) const
  { return !(*this == rhs); }
#endif
private:
  IntType value;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const bool inversive_congruential<IntType, a, b, p, val>::has_fixed_range;
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const typename inversive_congruential<IntType, a, b, p, val>::result_type inversive_congruential<IntType, a, b, p, val>::min_value;
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const typename inversive_congruential<IntType, a, b, p, val>::result_type inversive_congruential<IntType, a, b, p, val>::max_value;
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const typename inversive_congruential<IntType, a, b, p, val>::result_type inversive_congruential<IntType, a, b, p, val>::multiplier;
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const typename inversive_congruential<IntType, a, b, p, val>::result_type inversive_congruential<IntType, a, b, p, val>::increment;
template<class IntType, IntType a, IntType b, IntType p, IntType val>
const typename inversive_congruential<IntType, a, b, p, val>::result_type inversive_congruential<IntType, a, b, p, val>::modulus;
#endif

} // namespace random

typedef random::inversive_congruential<int32_t, 9102, 2147483647-36884165,
  2147483647, 0> hellekalek1995;

} // namespace boost

#endif // BOOST_RANDOM_INVERSIVE_CONGRUENTIAL_HPP
