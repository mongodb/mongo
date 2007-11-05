/* boost random/uniform_smallint.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: uniform_smallint.hpp,v 1.29 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-04-08  added min<max assertion (N. Becker)
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_UNIFORM_SMALLINT_HPP
#define BOOST_RANDOM_UNIFORM_SMALLINT_HPP

#include <cassert>
#include <iostream>
#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/random/uniform_01.hpp>
#include <boost/detail/workaround.hpp>
#ifdef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
#include <boost/type_traits/is_float.hpp>
#endif


namespace boost {

// uniform integer distribution on a small range [min, max]

namespace detail {

template <class InputStream, class UniformInt, class Impl>
InputStream& extract_uniform_int(InputStream& is, UniformInt& ud, Impl& impl)
{
    typename UniformInt::result_type min, max;
    is >> std::ws >> min >> std::ws >> max;
    impl.set(min, max);
    return is;
}

template<class UniformRandomNumberGenerator, class IntType>
struct uniform_smallint_integer
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef IntType result_type;

  uniform_smallint_integer(base_type & rng, IntType min, IntType max)
    : _rng(&rng)
  { set(min, max); }

  void set(result_type min, result_type max);
  
  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _min; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _max; }
  base_type& base() const { return *_rng; }
  
  result_type operator()()
  {
    // we must not use the low bits here, because LCGs get very bad then
    return (((*_rng)() - (_rng->min)()) / _factor) % _range + _min;
  }

private:
  typedef typename base_type::result_type base_result;
  base_type * _rng;
  IntType _min, _max;
  base_result _range;
  int _factor;
};

template<class UniformRandomNumberGenerator, class IntType>
void uniform_smallint_integer<UniformRandomNumberGenerator, IntType>::
set(result_type min, result_type max) 
{
  _min = min;
  _max = max;
  assert(min < max);

  _range = static_cast<base_result>(_max-_min)+1;
  base_result _factor = 1;
  
  // LCGs get bad when only taking the low bits.
  // (probably put this logic into a partial template specialization)
  // Check how many low bits we can ignore before we get too much
  // quantization error.
  base_result r_base = (_rng->max)() - (_rng->min)();
  if(r_base == (std::numeric_limits<base_result>::max)()) {
    _factor = 2;
    r_base /= 2;
  }
  r_base += 1;
  if(r_base % _range == 0) {
    // No quantization effects, good
    _factor = r_base / _range;
  } else {
    // carefully avoid overflow; pessimizing heree
    for( ; r_base/_range/32 >= _range; _factor *= 2)
      r_base /= 2;
  }
}

template<class UniformRandomNumberGenerator, class IntType>
class uniform_smallint_float
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef IntType result_type;

  uniform_smallint_float(base_type & rng, IntType min, IntType max)
    : _rng(rng)
  {
    // MSVC fails BOOST_STATIC_ASSERT with std::numeric_limits at class scope
#if !defined(BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS) && !(defined(BOOST_MSVC) && BOOST_MSVC <= 1300)
    BOOST_STATIC_ASSERT(std::numeric_limits<IntType>::is_integer);
    BOOST_STATIC_ASSERT(!std::numeric_limits<typename base_type::result_type>::is_integer);
#endif

    assert(min < max);
    set(min, max);
  }

  void set(result_type min, result_type max)
  {
    _min = min;
    _max = max;
    _range = static_cast<base_result>(_max-_min)+1;
  }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _min; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _max; }
  base_type& base() const { return _rng.base(); }

  result_type operator()()
  {
    return static_cast<IntType>(_rng() * _range) + _min;
  }

private:
  typedef typename base_type::result_type base_result;
  uniform_01<base_type> _rng;
  IntType _min, _max;
  base_result _range;
};


} // namespace detail




template<class IntType = int>
class uniform_smallint
{
public:
  typedef IntType input_type;
  typedef IntType result_type;

  explicit uniform_smallint(IntType min = 0, IntType max = 9)
    : _min(min), _max(max)
  {
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    // MSVC fails BOOST_STATIC_ASSERT with std::numeric_limits at class scope
    BOOST_STATIC_ASSERT(std::numeric_limits<IntType>::is_integer);
#endif
 }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _min; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return _max; }
  void reset() { }

  template<class Engine>
  result_type operator()(Engine& eng)
  {
    typedef typename Engine::result_type base_result;
    base_result _range = static_cast<base_result>(_max-_min)+1;
    base_result _factor = 1;
    
    // LCGs get bad when only taking the low bits.
    // (probably put this logic into a partial template specialization)
    // Check how many low bits we can ignore before we get too much
    // quantization error.
    base_result r_base = (eng.max)() - (eng.min)();
    if(r_base == (std::numeric_limits<base_result>::max)()) {
      _factor = 2;
      r_base /= 2;
    }
    r_base += 1;
    if(r_base % _range == 0) {
      // No quantization effects, good
      _factor = r_base / _range;
    } else {
      // carefully avoid overflow; pessimizing heree
      for( ; r_base/_range/32 >= _range; _factor *= 2)
        r_base /= 2;
    }

    return ((eng() - (eng.min)()) / _factor) % _range + _min;
  }

#if !defined(BOOST_NO_OPERATORS_IN_NAMESPACE) && !defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const uniform_smallint& ud)
  {
    os << ud._min << " " << ud._max;
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, uniform_smallint& ud)
  {
# if BOOST_WORKAROUND(_MSC_FULL_VER, BOOST_TESTED_AT(13102292)) && BOOST_MSVC > 1300
      return detail::extract_uniform_int(is, ud, ud._impl);
# else
    is >> std::ws >> ud._min >> std::ws >> ud._max;
    return is;
# endif
  }
#endif

private:
  result_type _min;
  result_type _max;
};

} // namespace boost

#endif // BOOST_RANDOM_UNIFORM_SMALLINT_HPP
