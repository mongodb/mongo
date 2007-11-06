/* boost random/exponential_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: exponential_distribution.hpp,v 1.16 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_EXPONENTIAL_DISTRIBUTION_HPP
#define BOOST_RANDOM_EXPONENTIAL_DISTRIBUTION_HPP

#include <cmath>
#include <cassert>
#include <iostream>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>

namespace boost {

// exponential distribution: p(x) = lambda * exp(-lambda * x)
template<class RealType = double>
class exponential_distribution
{
public:
  typedef RealType input_type;
  typedef RealType result_type;

#if !defined(BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS) && !(defined(BOOST_MSVC) && BOOST_MSVC <= 1300)
  BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

  explicit exponential_distribution(result_type lambda = result_type(1))
    : _lambda(lambda) { assert(lambda > result_type(0)); }

  // compiler-generated copy ctor and assignment operator are fine

  result_type lambda() const { return _lambda; }

  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  { 
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::log;
#endif
    return -result_type(1) / _lambda * log(result_type(1)-eng());
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const exponential_distribution& ed)
  {
    os << ed._lambda;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, exponential_distribution& ed)
  {
    is >> std::ws >> ed._lambda;
    return is;
  }
#endif

private:
  result_type _lambda;
};

} // namespace boost

#endif // BOOST_RANDOM_EXPONENTIAL_DISTRIBUTION_HPP
