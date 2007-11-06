/* boost random/poisson_distribution.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: poisson_distribution.hpp,v 1.15 2005/05/21 15:57:00 dgregor Exp $
 *
 */

#ifndef BOOST_RANDOM_POISSON_DISTRIBUTION_HPP
#define BOOST_RANDOM_POISSON_DISTRIBUTION_HPP

#include <cmath>
#include <cassert>
#include <iostream>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>

namespace boost {

// Knuth
template<class IntType = int, class RealType = double>
class poisson_distribution
{
public:
  typedef RealType input_type;
  typedef IntType result_type;

  explicit poisson_distribution(const RealType& mean = RealType(1))
    : _mean(mean)
  {
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    // MSVC fails BOOST_STATIC_ASSERT with std::numeric_limits at class scope
    BOOST_STATIC_ASSERT(std::numeric_limits<IntType>::is_integer);
    BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

    assert(mean > RealType(0));
    init();
  }

  // compiler-generated copy ctor and assignment operator are fine

  RealType mean() const { return _mean; }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
    // TODO: This is O(_mean), but it should be O(log(_mean)) for large _mean
    RealType product = RealType(1);
    for(result_type m = 0; ; ++m) {
      product *= eng();
      if(product <= _exp_mean)
        return m;
    }
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const poisson_distribution& pd)
  {
    os << pd._mean;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, poisson_distribution& pd)
  {
    is >> std::ws >> pd._mean;
    pd.init();
    return is;
  }
#endif

private:
  void init()
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    // allow for Koenig lookup
    using std::exp;
#endif
    _exp_mean = exp(-_mean);
  }

  RealType _mean;
  // some precomputed data from the parameters
  RealType _exp_mean;
};

} // namespace boost

#endif // BOOST_RANDOM_POISSON_DISTRIBUTION_HPP
