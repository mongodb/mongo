/* boost random/bernoulli_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: bernoulli_distribution.hpp,v 1.19 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_BERNOULLI_DISTRIBUTION_HPP
#define BOOST_RANDOM_BERNOULLI_DISTRIBUTION_HPP

#include <cassert>
#include <iostream>

namespace boost {

// Bernoulli distribution: p(true) = p, p(false) = 1-p   (boolean)
template<class RealType = double>
class bernoulli_distribution
{
public:
  // In principle, this could work with both integer and floating-point
  // types.  Generating floating-point random numbers in the first
  // place is probably more expensive, so use integer as input.
  typedef int input_type;
  typedef bool result_type;

  explicit bernoulli_distribution(const RealType& p = RealType(0.5)) 
    : _p(p)
  {
    assert(p >= 0);
    assert(p <= 1);
  }

  // compiler-generated copy ctor and assignment operator are fine

  RealType p() const { return _p; }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
    if(_p == RealType(0))
      return false;
    else
      return RealType(eng() - (eng.min)()) <= _p * RealType((eng.max)()-(eng.min)());
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const bernoulli_distribution& bd)
  {
    os << bd._p;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, bernoulli_distribution& bd)
  {
    is >> std::ws >> bd._p;
    return is;
  }
#endif

private:
  RealType _p;
};

} // namespace boost

#endif // BOOST_RANDOM_BERNOULLI_DISTRIBUTION_HPP
