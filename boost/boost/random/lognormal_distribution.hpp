/* boost random/lognormal_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: lognormal_distribution.hpp,v 1.16 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_LOGNORMAL_DISTRIBUTION_HPP
#define BOOST_RANDOM_LOGNORMAL_DISTRIBUTION_HPP

#include <cmath>      // std::exp, std::sqrt
#include <cassert>
#include <iostream>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/normal_distribution.hpp>

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std {
  using ::log;
  using ::sqrt;
}
#endif

namespace boost {

#if defined(__GNUC__) && (__GNUC__ < 3)
// Special gcc workaround: gcc 2.95.x ignores using-declarations
// in template classes (confirmed by gcc author Martin v. Loewis)
  using std::sqrt;
  using std::exp;
#endif

template<class RealType = double>
class lognormal_distribution
{
public:
  typedef typename normal_distribution<RealType>::input_type input_type;
  typedef RealType result_type;

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

  explicit lognormal_distribution(result_type mean = result_type(1),
                                  result_type sigma = result_type(1))
    : _mean(mean), _sigma(sigma)
  { 
    assert(mean > result_type(0));
    init();
  }

  // compiler-generated copy ctor and assignment operator are fine

  RealType& mean() const { return _mean; }
  RealType& sigma() const { return _sigma; }
  void reset() { _normal.reset(); }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    // allow for Koenig lookup
    using std::exp;
#endif
    return exp(_normal(eng) * _nsigma + _nmean);
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const lognormal_distribution& ld)
  {
    os << ld._normal << " " << ld._mean << " " << ld._sigma;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, lognormal_distribution& ld)
  {
    is >> std::ws >> ld._normal >> std::ws >> ld._mean >> std::ws >> ld._sigma;
    ld.init();
    return is;
  }
#endif

private:
  void init()
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    // allow for Koenig lookup
    using std::exp; using std::log; using std::sqrt;
#endif
    _nmean = log(_mean*_mean/sqrt(_sigma*_sigma + _mean*_mean));
    _nsigma = sqrt(log(_sigma*_sigma/_mean/_mean+result_type(1)));
  }

  RealType _mean, _sigma;
  RealType _nmean, _nsigma;
  normal_distribution<result_type> _normal;
};

} // namespace boost

#endif // BOOST_RANDOM_LOGNORMAL_DISTRIBUTION_HPP
