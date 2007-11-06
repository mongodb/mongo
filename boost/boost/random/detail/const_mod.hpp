/* boost random/detail/const_mod.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: const_mod.hpp,v 1.8 2004/07/27 03:43:32 dgregor Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_CONST_MOD_HPP
#define BOOST_RANDOM_CONST_MOD_HPP

#include <cassert>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/integer_traits.hpp>
#include <boost/detail/workaround.hpp>

namespace boost {
namespace random {

/*
 * Some random number generators require modular arithmetic.  Put
 * everything we need here.
 * IntType must be an integral type.
 */

namespace detail {

  template<bool is_signed>
  struct do_add
  { };

  template<>
  struct do_add<true>
  {
    template<class IntType>
    static IntType add(IntType m, IntType x, IntType c)
    {
      x += (c-m);
      if(x < 0)
        x += m;
      return x;
    }
  };

  template<>
  struct do_add<false>
  {
    template<class IntType>
    static IntType add(IntType, IntType, IntType)
    {
      // difficult
      assert(!"const_mod::add with c too large");
      return 0;
    }
  };
} // namespace detail

#if !(defined(__BORLANDC__) && (__BORLANDC__ == 0x560))

template<class IntType, IntType m>
class const_mod
{
public:
  static IntType add(IntType x, IntType c)
  {
    if(c == 0)
      return x;
    else if(c <= traits::const_max - m)    // i.e. m+c < max
      return add_small(x, c);
    else
      return detail::do_add<traits::is_signed>::add(m, x, c);
  }

  static IntType mult(IntType a, IntType x)
  {
    if(a == 1)
      return x;
    else if(m <= traits::const_max/a)      // i.e. a*m <= max
      return mult_small(a, x);
    else if(traits::is_signed && (m%a < m/a))
      return mult_schrage(a, x);
    else {
      // difficult
      assert(!"const_mod::mult with a too large");
      return 0;
    }
  }

  static IntType mult_add(IntType a, IntType x, IntType c)
  {
    if(m <= (traits::const_max-c)/a)   // i.e. a*m+c <= max
      return (a*x+c) % m;
    else
      return add(mult(a, x), c);
  }

  static IntType invert(IntType x)
  { return x == 0 ? 0 : invert_euclidian(x); }

private:
  typedef integer_traits<IntType> traits;

  const_mod();      // don't instantiate

  static IntType add_small(IntType x, IntType c)
  {
    x += c;
    if(x >= m)
      x -= m;
    return x;
  }

  static IntType mult_small(IntType a, IntType x)
  {
    return a*x % m;
  }

  static IntType mult_schrage(IntType a, IntType value)
  {
    const IntType q = m / a;
    const IntType r = m % a;

    assert(r < q);        // check that overflow cannot happen

    value = a*(value%q) - r*(value/q);
    // An optimizer bug in the SGI MIPSpro 7.3.1.x compiler requires this
    // convoluted formulation of the loop (Synge Todo)
    for(;;) {
      if (value > 0)
        break;
      value += m;
    }
    return value;
  }

  // invert c in the finite field (mod m) (m must be prime)
  static IntType invert_euclidian(IntType c)
  {
    // we are interested in the gcd factor for c, because this is our inverse
    BOOST_STATIC_ASSERT(m > 0);
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3003))
    assert(boost::integer_traits<IntType>::is_signed);
#elif !defined(BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS)
    BOOST_STATIC_ASSERT(boost::integer_traits<IntType>::is_signed);
#endif
    assert(c > 0);
    IntType l1 = 0;
    IntType l2 = 1;
    IntType n = c;
    IntType p = m;
    for(;;) {
      IntType q = p / n;
      l1 -= q * l2;           // this requires a signed IntType!
      p -= q * n;
      if(p == 0)
        return (l2 < 1 ? l2 + m : l2);
      IntType q2 = n / p;
      l2 -= q2 * l1;
      n -= q2 * p;
      if(n == 0)
        return (l1 < 1 ? l1 + m : l1);
    }
  }
};

// The modulus is exactly the word size: rely on machine overflow handling.
// Due to a GCC bug, we cannot partially specialize in the presence of
// template value parameters.
template<>
class const_mod<unsigned int, 0>
{
  typedef unsigned int IntType;
public:
  static IntType add(IntType x, IntType c) { return x+c; }
  static IntType mult(IntType a, IntType x) { return a*x; }
  static IntType mult_add(IntType a, IntType x, IntType c) { return a*x+c; }

  // m is not prime, thus invert is not useful
private:                      // don't instantiate
  const_mod();
};

template<>
class const_mod<unsigned long, 0>
{
  typedef unsigned long IntType;
public:
  static IntType add(IntType x, IntType c) { return x+c; }
  static IntType mult(IntType a, IntType x) { return a*x; }
  static IntType mult_add(IntType a, IntType x, IntType c) { return a*x+c; }

  // m is not prime, thus invert is not useful
private:                      // don't instantiate
  const_mod();
};

// the modulus is some power of 2: rely partly on machine overflow handling
// we only specialize for rand48 at the moment
#ifndef BOOST_NO_INT64_T
template<>
class const_mod<uint64_t, uint64_t(1) << 48>
{
  typedef uint64_t IntType;
public:
  static IntType add(IntType x, IntType c) { return c == 0 ? x : mod(x+c); }
  static IntType mult(IntType a, IntType x) { return mod(a*x); }
  static IntType mult_add(IntType a, IntType x, IntType c)
    { return mod(a*x+c); }
  static IntType mod(IntType x) { return x &= ((uint64_t(1) << 48)-1); }

  // m is not prime, thus invert is not useful
private:                      // don't instantiate
  const_mod();
};
#endif /* !BOOST_NO_INT64_T */

#else

//
// for some reason Borland C++ Builder 6 has problems with
// the full specialisations of const_mod, define a generic version
// instead, the compiler will optimise away the const-if statements:
//

template<class IntType, IntType m>
class const_mod
{
public:
  static IntType add(IntType x, IntType c)
  {
    if(0 == m)
    {
       return x+c;
    }
    else
    {
       if(c == 0)
         return x;
       else if(c <= traits::const_max - m)    // i.e. m+c < max
         return add_small(x, c);
       else
         return detail::do_add<traits::is_signed>::add(m, x, c);
    }
  }

  static IntType mult(IntType a, IntType x)
  {
    if(x == 0)
    {
       return a*x;
    }
    else
    {
       if(a == 1)
         return x;
       else if(m <= traits::const_max/a)      // i.e. a*m <= max
         return mult_small(a, x);
       else if(traits::is_signed && (m%a < m/a))
         return mult_schrage(a, x);
       else {
         // difficult
         assert(!"const_mod::mult with a too large");
         return 0;
       }
    }
  }

  static IntType mult_add(IntType a, IntType x, IntType c)
  {
    if(m == 0)
    {
       return a*x+c;
    }
    else
    {
       if(m <= (traits::const_max-c)/a)   // i.e. a*m+c <= max
         return (a*x+c) % m;
       else
         return add(mult(a, x), c);
    }
  }

  static IntType invert(IntType x)
  { return x == 0 ? 0 : invert_euclidian(x); }

private:
  typedef integer_traits<IntType> traits;

  const_mod();      // don't instantiate

  static IntType add_small(IntType x, IntType c)
  {
    x += c;
    if(x >= m)
      x -= m;
    return x;
  }

  static IntType mult_small(IntType a, IntType x)
  {
    return a*x % m;
  }

  static IntType mult_schrage(IntType a, IntType value)
  {
    const IntType q = m / a;
    const IntType r = m % a;

    assert(r < q);        // check that overflow cannot happen

    value = a*(value%q) - r*(value/q);
    while(value <= 0)
      value += m;
    return value;
  }

  // invert c in the finite field (mod m) (m must be prime)
  static IntType invert_euclidian(IntType c)
  {
    // we are interested in the gcd factor for c, because this is our inverse
    BOOST_STATIC_ASSERT(m > 0);
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(boost::integer_traits<IntType>::is_signed);
#endif
    assert(c > 0);
    IntType l1 = 0;
    IntType l2 = 1;
    IntType n = c;
    IntType p = m;
    for(;;) {
      IntType q = p / n;
      l1 -= q * l2;           // this requires a signed IntType!
      p -= q * n;
      if(p == 0)
        return (l2 < 1 ? l2 + m : l2);
      IntType q2 = n / p;
      l2 -= q2 * l1;
      n -= q2 * p;
      if(n == 0)
        return (l1 < 1 ? l1 + m : l1);
    }
  }
};


#endif

} // namespace random
} // namespace boost

#endif // BOOST_RANDOM_CONST_MOD_HPP
