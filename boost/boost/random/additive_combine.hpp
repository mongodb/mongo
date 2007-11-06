/* boost random/additive_combine.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: additive_combine.hpp,v 1.11 2005/05/21 15:57:00 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_ADDITIVE_COMBINE_HPP
#define BOOST_RANDOM_ADDITIVE_COMBINE_HPP

#include <iostream>
#include <algorithm> // for std::min and std::max
#include <boost/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/random/linear_congruential.hpp>

namespace boost {
namespace random {

// L'Ecuyer 1988
template<class MLCG1, class MLCG2,
#ifndef BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
  typename MLCG1::result_type 
#else
  int32_t
#endif
  val>
class additive_combine
{
public:
  typedef MLCG1 first_base;
  typedef MLCG2 second_base;
  typedef typename MLCG1::result_type result_type;
#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
  static const bool has_fixed_range = true;
  static const result_type min_value = 1;
  static const result_type max_value = MLCG1::max_value-1;
#else
  enum { has_fixed_range = false };
#endif
  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return 1; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (_mlcg1.max)()-1; }

  additive_combine() : _mlcg1(), _mlcg2() { }
  additive_combine(typename MLCG1::result_type seed1, 
                   typename MLCG2::result_type seed2)
    : _mlcg1(seed1), _mlcg2(seed2) { }
  template<class It> additive_combine(It& first, It last)
    : _mlcg1(first, last), _mlcg2(first, last) { }

  void seed()
  {
    _mlcg1.seed();
    _mlcg2.seed();
  }

  void seed(typename MLCG1::result_type seed1,
            typename MLCG2::result_type seed2)
  {
    _mlcg1(seed1);
    _mlcg2(seed2);
  }

  template<class It> void seed(It& first, It last)
  {
    _mlcg1.seed(first, last);
    _mlcg2.seed(first, last);
  }

  result_type operator()() {
    result_type z = _mlcg1() - _mlcg2();
    if(z < 1)
      z += MLCG1::modulus-1;
    return z;
  }
  static bool validation(result_type x) { return val == x; }

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const additive_combine& r)
  { os << r._mlcg1 << " " << r._mlcg2; return os; }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, additive_combine& r)
  { is >> r._mlcg1 >> std::ws >> r._mlcg2; return is; }
#endif

  friend bool operator==(const additive_combine& x, const additive_combine& y)
  { return x._mlcg1 == y._mlcg1 && x._mlcg2 == y._mlcg2; }
  friend bool operator!=(const additive_combine& x, const additive_combine& y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(const additive_combine& rhs) const
  { return _mlcg1 == rhs._mlcg1 && _mlcg2 == rhs._mlcg2; }
  bool operator!=(const additive_combine& rhs) const
  { return !(*this == rhs); }
#endif
private:
  MLCG1 _mlcg1;
  MLCG2 _mlcg2;
};

} // namespace random

typedef random::additive_combine<
    random::linear_congruential<int32_t, 40014, 0, 2147483563, 0>,
    random::linear_congruential<int32_t, 40692, 0, 2147483399, 0>,
  2060321752> ecuyer1988;

} // namespace boost

#endif // BOOST_RANDOM_ADDITIVE_COMBINE_HPP
