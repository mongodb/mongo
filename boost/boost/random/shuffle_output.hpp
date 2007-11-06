/* boost random/shuffle_output.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: shuffle_output.hpp,v 1.10 2005/08/25 16:27:22 johnmaddock Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_SHUFFLE_OUTPUT_HPP
#define BOOST_RANDOM_SHUFFLE_OUTPUT_HPP

#include <iostream>
#include <algorithm>     // std::copy
#include <cassert>
#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/random/linear_congruential.hpp>

namespace boost {
namespace random {

// Carter Bays and S.D. Durham 1979
template<class UniformRandomNumberGenerator, int k,
#ifndef BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
  typename UniformRandomNumberGenerator::result_type 
#else
  uint32_t
#endif
  val = 0>
class shuffle_output
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef typename base_type::result_type result_type;

  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);
  BOOST_STATIC_CONSTANT(int, buffer_size = k);

  shuffle_output() : _rng() { init(); }
#if defined(BOOST_MSVC) && _MSC_VER < 1300
  // MSVC does not implicitly generate the copy constructor here
  shuffle_output(const shuffle_output & x)
    : _rng(x._rng), y(x.y) { std::copy(x.v, x.v+k, v); }
#endif
  template<class T>
  explicit shuffle_output(T seed) : _rng(seed) { init(); }
  explicit shuffle_output(const base_type & rng) : _rng(rng) { init(); }
  template<class It> shuffle_output(It& first, It last)
    : _rng(first, last) { init(); }
  void seed() { _rng.seed(); init(); }
  template<class T>
  void seed(T s) { _rng.seed(s); init(); }
  template<class It> void seed(It& first, It last)
  {
    _rng.seed(first, last);
    init();
  }

  const base_type& base() const { return _rng; }

  result_type operator()() {
    // calculating the range every time may seem wasteful.  However, this
    // makes the information locally available for the optimizer.
    result_type range = (max)()-(min)()+1;
    int j = k*(y-(min)())/range;
    // assert(0 <= j && j < k);
    y = v[j];
    v[j] = _rng();
    return y;
  }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (_rng.min)(); }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (_rng.max)(); }
  static bool validation(result_type x) { return val == x; }

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const shuffle_output& s)
  {
    os << s._rng << " " << s.y << " ";
    for(int i = 0; i < s.buffer_size; ++i)
      os << s.v[i] << " ";
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, shuffle_output& s)
  {
    is >> s._rng >> std::ws >> s.y >> std::ws;
    for(int i = 0; i < s.buffer_size; ++i)
      is >> s.v[i] >> std::ws;
    return is;
  }
#endif

  friend bool operator==(const shuffle_output& x, const shuffle_output& y)
  { return x._rng == y._rng && x.y == y.y && std::equal(x.v, x.v+k, y.v); }
  friend bool operator!=(const shuffle_output& x, const shuffle_output& y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(const shuffle_output& rhs) const
  { return _rng == rhs._rng && y == rhs.y && std::equal(v, v+k, rhs.v); }
  bool operator!=(const shuffle_output& rhs) const
  { return !(*this == rhs); }
#endif
private:
  void init()
  {
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(std::numeric_limits<result_type>::is_integer);
#endif
    result_type range = (max)()-(min)();
    assert(range > 0);      // otherwise there would be little choice
    if(static_cast<unsigned long>(k * range) < 
       static_cast<unsigned long>(range))  // not a sufficient condition
      // likely overflow with bucket number computation
      assert(!"overflow will occur");

    // we cannot use std::generate, because it uses pass-by-value for _rng
    for(result_type * p = v; p != v+k; ++p)
      *p = _rng();
    y = _rng();
  }

  base_type _rng;
  result_type v[k];
  result_type y;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class UniformRandomNumberGenerator, int k, 
#ifndef BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
  typename UniformRandomNumberGenerator::result_type 
#else
  uint32_t
#endif
  val>
const bool shuffle_output<UniformRandomNumberGenerator, k, val>::has_fixed_range;

template<class UniformRandomNumberGenerator, int k, 
#ifndef BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
  typename UniformRandomNumberGenerator::result_type 
#else
  uint32_t
#endif
  val>
const int shuffle_output<UniformRandomNumberGenerator, k, val>::buffer_size;
#endif

} // namespace random

// validation by experiment from Harry Erwin's generator.h (private e-mail)
typedef random::shuffle_output<
    random::linear_congruential<uint32_t, 1366, 150889, 714025, 0>,
  97, 139726> kreutzer1986;


} // namespace boost

#endif // BOOST_RANDOM_SHUFFLE_OUTPUT_HPP
