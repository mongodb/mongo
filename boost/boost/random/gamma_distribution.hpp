/* boost random/gamma_distribution.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: gamma_distribution.hpp,v 1.9 2004/07/27 03:43:32 dgregor Exp $
 *
 */

#ifndef BOOST_RANDOM_GAMMA_DISTRIBUTION_HPP
#define BOOST_RANDOM_GAMMA_DISTRIBUTION_HPP

#include <cmath>
#include <cassert>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/exponential_distribution.hpp>

namespace boost {

// Knuth
template<class RealType = double>
class gamma_distribution
{
public:
  typedef RealType input_type;
  typedef RealType result_type;

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
  BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

  explicit gamma_distribution(const result_type& alpha = result_type(1))
    : _exp(result_type(1)), _alpha(alpha)
  {
    assert(alpha > result_type(0));
    init();
  }

  // compiler-generated copy ctor and assignment operator are fine

  RealType alpha() const { return _alpha; }

  void reset() { _exp.reset(); }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    // allow for Koenig lookup
    using std::tan; using std::sqrt; using std::exp; using std::log;
    using std::pow;
#endif
    if(_alpha == result_type(1)) {
      return _exp(eng);
    } else if(_alpha > result_type(1)) {
      // Can we have a boost::mathconst please?
      const result_type pi = result_type(3.14159265358979323846);
      for(;;) {
        result_type y = tan(pi * eng());
        result_type x = sqrt(result_type(2)*_alpha-result_type(1))*y
          + _alpha-result_type(1);
        if(x <= result_type(0))
          continue;
        if(eng() >
           (result_type(1)+y*y) * exp((_alpha-result_type(1))
                                        *log(x/(_alpha-result_type(1)))
                                        - sqrt(result_type(2)*_alpha
                                               -result_type(1))*y))
          continue;
        return x;
      }
    } else /* alpha < 1.0 */ {
      for(;;) {
        result_type u = eng();
        result_type y = _exp(eng);
        result_type x, q;
        if(u < _p) {
          x = exp(-y/_alpha);
          q = _p*exp(-x);
        } else {
          x = result_type(1)+y;
          q = _p + (result_type(1)-_p) * pow(x, _alpha-result_type(1));
        }
        if(u >= q)
          continue;
        return x;
      }
    }
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const gamma_distribution& gd)
  {
    os << gd._alpha;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, gamma_distribution& gd)
  {
    is >> std::ws >> gd._alpha;
    gd.init();
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
    _p = exp(result_type(1)) / (_alpha + exp(result_type(1)));
  }

  exponential_distribution<RealType> _exp;
  result_type _alpha;
  // some data precomputed from the parameters
  result_type _p;
};

} // namespace boost

#endif // BOOST_RANDOM_GAMMA_DISTRIBUTION_HPP
