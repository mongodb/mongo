/* boost random/geometric_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: geometric_distribution.hpp,v 1.16 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_GEOMETRIC_DISTRIBUTION_HPP
#define BOOST_RANDOM_GEOMETRIC_DISTRIBUTION_HPP

#include <cmath>          // std::log
#include <cassert>
#include <iostream>
#include <boost/random/uniform_01.hpp>

namespace boost {

#if defined(__GNUC__) && (__GNUC__ < 3)
// Special gcc workaround: gcc 2.95.x ignores using-declarations
// in template classes (confirmed by gcc author Martin v. Loewis)
  using std::log;
#endif

// geometric distribution: p(i) = (1-p) * pow(p, i-1)   (integer)
template<class IntType = int, class RealType = double>
class geometric_distribution
{
public:
  typedef RealType input_type;
  typedef IntType result_type;

  explicit geometric_distribution(const RealType& p = RealType(0.5))
    : _p(p)
  {
    assert(RealType(0) < p && p < RealType(1));
    init();
  }

  // compiler-generated copy ctor and assignment operator are fine

  RealType p() const { return _p; }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::log;
    using std::floor;
#endif
    return IntType(floor(log(RealType(1)-eng()) / _log_p)) + IntType(1);
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const geometric_distribution& gd)
  {
    os << gd._p;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, geometric_distribution& gd)
  {
    is >> std::ws >> gd._p;
    gd.init();
    return is;
  }
#endif

private:
  void init()
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::log;
#endif
    _log_p = log(_p);
  }

  RealType _p;
  RealType _log_p;
};

} // namespace boost

#endif // BOOST_RANDOM_GEOMETRIC_DISTRIBUTION_HPP

