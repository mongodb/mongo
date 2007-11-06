/* boost random/uniform_01.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: uniform_01.hpp,v 1.18 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_UNIFORM_01_HPP
#define BOOST_RANDOM_UNIFORM_01_HPP

#include <iostream>
#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>

namespace boost {

// Because it is so commonly used: uniform distribution on the real [0..1)
// range.  This allows for specializations to avoid a costly int -> float
// conversion plus float multiplication
template<class UniformRandomNumberGenerator, class RealType = double>
class uniform_01
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef RealType result_type;

  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);

#if !defined(BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS) && !(defined(BOOST_MSVC) && BOOST_MSVC <= 1300)
  BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

  explicit uniform_01(base_type rng)
    : _rng(rng),
      _factor(result_type(1) /
              (result_type((_rng.max)()-(_rng.min)()) +
               result_type(std::numeric_limits<base_result>::is_integer ? 1 : 0)))
  {
  }
  // compiler-generated copy ctor and copy assignment are fine

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return result_type(0); }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return result_type(1); }
  base_type& base() { return _rng; }
  const base_type& base() const { return _rng; }
  void reset() { }

  result_type operator()() {
    return result_type(_rng() - (_rng.min)()) * _factor;
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const uniform_01& u)
  {
    os << u._rng;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, uniform_01& u)
  {
    is >> u._rng;
    return is;
  }
#endif

private:
  typedef typename base_type::result_type base_result;
  base_type _rng;
  result_type _factor;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class UniformRandomNumberGenerator, class RealType>
const bool uniform_01<UniformRandomNumberGenerator, RealType>::has_fixed_range;
#endif

} // namespace boost

#endif // BOOST_RANDOM_UNIFORM_01_HPP
