/* boost random/binomial_distribution.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: binomial_distribution.hpp,v 1.10 2005/06/24 22:13:43 jmaurer Exp $
 *
 */

#ifndef BOOST_RANDOM_BINOMIAL_DISTRIBUTION_HPP
#define BOOST_RANDOM_BINOMIAL_DISTRIBUTION_HPP

#include <cmath>
#include <cassert>
#include <boost/random/bernoulli_distribution.hpp>

namespace boost {

// Knuth
template<class IntType = int, class RealType = double>
class binomial_distribution
{
public:
  typedef typename bernoulli_distribution<RealType>::input_type input_type;
  typedef IntType result_type;

  explicit binomial_distribution(IntType t = 1,
                                 const RealType& p = RealType(0.5))
    : _bernoulli(p), _t(t)
  {
    assert(t >= 0);
    assert(RealType(0) <= 0 && p <= RealType(1));
  }

  // compiler-generated copy ctor and assignment operator are fine

  IntType t() const { return _t; }
  RealType p() const { return _bernoulli.p(); }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
    // TODO: This is O(_t), but it should be O(log(_t)) for large _t
    result_type n = 0;
    for(IntType i = 0; i < _t; ++i)
      if(_bernoulli(eng))
        ++n;
    return n;
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const binomial_distribution& bd)
  {
    os << bd._bernoulli << " " << bd._t;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, binomial_distribution& bd)
  {
    is >> std::ws >> bd._bernoulli >> std::ws >> bd._t;
    return is;
  }
#endif

private:
  bernoulli_distribution<RealType> _bernoulli;
  IntType _t;
};

} // namespace boost

#endif // BOOST_RANDOM_BINOMIAL_DISTRIBUTION_HPP
