/* boost random/triangle_distribution.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: triangle_distribution.hpp,v 1.11 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_TRIANGLE_DISTRIBUTION_HPP
#define BOOST_RANDOM_TRIANGLE_DISTRIBUTION_HPP

#include <cmath>
#include <cassert>
#include <boost/random/uniform_01.hpp>

namespace boost {

// triangle distribution, with a smallest, b most probable, and c largest
// value.
template<class RealType = double>
class triangle_distribution
{
public:
  typedef RealType input_type;
  typedef RealType result_type;

  explicit triangle_distribution(result_type a = result_type(0),
                                 result_type b = result_type(0.5),
                                 result_type c = result_type(1))
    : _a(a), _b(b), _c(c)
  {
    assert(_a <= _b && _b <= _c);
    init();
  }

  // compiler-generated copy ctor and assignment operator are fine
  result_type a() const { return _a; }
  result_type b() const { return _b; }
  result_type c() const { return _c; }

  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sqrt;
#endif
    result_type u = eng();
    if( u <= q1 )
      return _a + p1*sqrt(u);
    else
      return _c - d3*sqrt(d2*u-d1);
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const triangle_distribution& td)
  {
    os << td._a << " " << td._b << " " << td._c;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, triangle_distribution& td)
  {
    is >> std::ws >> td._a >> std::ws >> td._b >> std::ws >> td._c;
    td.init();
    return is;
  }
#endif

private:
  void init()
  {
#ifndef BOOST_NO_STDC_NAMESPACE
    using std::sqrt;
#endif
    d1 = _b - _a;
    d2 = _c - _a;
    d3 = sqrt(_c - _b);
    q1 = d1 / d2;
    p1 = sqrt(d1 * d2);
  }

  result_type _a, _b, _c;
  result_type d1, d2, d3, q1, p1;
};

} // namespace boost

#endif // BOOST_RANDOM_TRIANGLE_DISTRIBUTION_HPP
