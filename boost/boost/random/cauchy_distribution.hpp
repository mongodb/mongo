/* boost random/cauchy_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: cauchy_distribution.hpp,v 1.15 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_CAUCHY_DISTRIBUTION_HPP
#define BOOST_RANDOM_CAUCHY_DISTRIBUTION_HPP

#include <cmath>
#include <iostream>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>

namespace boost {

#if defined(__GNUC__) && (__GNUC__ < 3)
// Special gcc workaround: gcc 2.95.x ignores using-declarations
// in template classes (confirmed by gcc author Martin v. Loewis)
  using std::tan;
#endif

// Cauchy distribution: p(x) = sigma/(pi*(sigma**2 + (x-median)**2))
template<class RealType = double>
class cauchy_distribution
{
public:
  typedef RealType input_type;
  typedef RealType result_type;

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
  BOOST_STATIC_ASSERT(!std::numeric_limits<RealType>::is_integer);
#endif

  explicit cauchy_distribution(result_type median = result_type(0), 
                               result_type sigma = result_type(1))
    : _median(median), _sigma(sigma) { }

  // compiler-generated copy ctor and assignment operator are fine

  result_type median() const { return _median; }
  result_type sigma() const { return _sigma; }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
    // Can we have a boost::mathconst please?
    const result_type pi = result_type(3.14159265358979323846);
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::tan;
#endif
    return _median + _sigma * tan(pi*(eng()-result_type(0.5)));
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const cauchy_distribution& cd)
  {
    os << cd._median << " " << cd._sigma;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, cauchy_distribution& cd)
  {
    is >> std::ws >> cd._median >> std::ws >> cd._sigma;
    return is;
  }
#endif

private:
  result_type _median, _sigma;
};

} // namespace boost

#endif // BOOST_RANDOM_CAUCHY_DISTRIBUTION_HPP
