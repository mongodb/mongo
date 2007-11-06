/* boost random/mersenne_twister.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: mersenne_twister.hpp,v 1.20 2005/07/21 22:04:31 jmaurer Exp $
 *
 * Revision history
 *  2001-02-18  moved to individual header files
 */

#ifndef BOOST_RANDOM_MERSENNE_TWISTER_HPP
#define BOOST_RANDOM_MERSENNE_TWISTER_HPP

#include <iostream>
#include <algorithm>     // std::copy
#include <stdexcept>
#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>
#include <boost/integer_traits.hpp>
#include <boost/cstdint.hpp>
#include <boost/random/linear_congruential.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/random/detail/ptr_helper.hpp>

namespace boost {
namespace random {

// http://www.math.keio.ac.jp/matumoto/emt.html
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
class mersenne_twister
{
public:
  typedef UIntType result_type;
  BOOST_STATIC_CONSTANT(int, word_size = w);
  BOOST_STATIC_CONSTANT(int, state_size = n);
  BOOST_STATIC_CONSTANT(int, shift_size = m);
  BOOST_STATIC_CONSTANT(int, mask_bits = r);
  BOOST_STATIC_CONSTANT(UIntType, parameter_a = a);
  BOOST_STATIC_CONSTANT(int, output_u = u);
  BOOST_STATIC_CONSTANT(int, output_s = s);
  BOOST_STATIC_CONSTANT(UIntType, output_b = b);
  BOOST_STATIC_CONSTANT(int, output_t = t);
  BOOST_STATIC_CONSTANT(UIntType, output_c = c);
  BOOST_STATIC_CONSTANT(int, output_l = l);

  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);
  
  mersenne_twister() { seed(); }

#if defined(__SUNPRO_CC) && (__SUNPRO_CC <= 0x520)
  // Work around overload resolution problem (Gennadiy E. Rozental)
  explicit mersenne_twister(const UIntType& value)
#else
  explicit mersenne_twister(UIntType value)
#endif
  { seed(value); }
  template<class It> mersenne_twister(It& first, It last) { seed(first,last); }

  template<class Generator>
  explicit mersenne_twister(Generator & gen) { seed(gen); }

  // compiler-generated copy ctor and assignment operator are fine

  void seed() { seed(UIntType(5489)); }

#if defined(__SUNPRO_CC) && (__SUNPRO_CC <= 0x520)
  // Work around overload resolution problem (Gennadiy E. Rozental)
  void seed(const UIntType& value)
#else
  void seed(UIntType value)
#endif
  {
    // New seeding algorithm from 
    // http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/MT2002/emt19937ar.html
    // In the previous versions, MSBs of the seed affected only MSBs of the
    // state x[].
    const UIntType mask = ~0u;
    x[0] = value & mask;
    for (i = 1; i < n; i++) {
      // See Knuth "The Art of Computer Programming" Vol. 2, 3rd ed., page 106
      x[i] = (1812433253UL * (x[i-1] ^ (x[i-1] >> (w-2))) + i) & mask;
    }
  }

  // For GCC, moving this function out-of-line prevents inlining, which may
  // reduce overall object code size.  However, MSVC does not grok
  // out-of-line definitions of member function templates.
  template<class Generator>
  void seed(Generator & gen)
  {
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
    BOOST_STATIC_ASSERT(!std::numeric_limits<result_type>::is_signed);
#endif
    // I could have used std::generate_n, but it takes "gen" by value
    for(int j = 0; j < n; j++)
      x[j] = gen();
    i = n;
  }

  template<class It>
  void seed(It& first, It last)
  {
    int j;
    for(j = 0; j < n && first != last; ++j, ++first)
      x[j] = *first;
    i = n;
    if(first == last && j < n)
      throw std::invalid_argument("mersenne_twister::seed");
  }
  
  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return 0; }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const
  {
    // avoid "left shift count >= with of type" warning
    result_type res = 0;
    for(int i = 0; i < w; ++i)
      res |= (1u << i);
    return res;
  }

  result_type operator()();
  static bool validation(result_type v) { return val == v; }

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const mersenne_twister& mt)
  {
    for(int j = 0; j < mt.state_size; ++j)
      os << mt.compute(j) << " ";
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, mersenne_twister& mt)
  {
    for(int j = 0; j < mt.state_size; ++j)
      is >> mt.x[j] >> std::ws;
    // MSVC (up to 7.1) and Borland (up to 5.64) don't handle the template
    // value parameter "n" available from the class template scope, so use
    // the static constant with the same value
    mt.i = mt.state_size;
    return is;
  }
#endif

  friend bool operator==(const mersenne_twister& x, const mersenne_twister& y)
  {
    for(int j = 0; j < state_size; ++j)
      if(x.compute(j) != y.compute(j))
        return false;
    return true;
  }

  friend bool operator!=(const mersenne_twister& x, const mersenne_twister& y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(const mersenne_twister& rhs) const
  {
    for(int j = 0; j < state_size; ++j)
      if(compute(j) != rhs.compute(j))
        return false;
    return true;
  }

  bool operator!=(const mersenne_twister& rhs) const
  { return !(*this == rhs); }
#endif

private:
  // returns x(i-n+index), where index is in 0..n-1
  UIntType compute(unsigned int index) const
  {
    // equivalent to (i-n+index) % 2n, but doesn't produce negative numbers
    return x[ (i + n + index) % (2*n) ];
  }
  void twist(int block);

  // state representation: next output is o(x(i))
  //   x[0]  ... x[k] x[k+1] ... x[n-1]     x[n]     ... x[2*n-1]   represents
  //  x(i-k) ... x(i) x(i+1) ... x(i-k+n-1) x(i-k-n) ... x[i(i-k-1)]
  // The goal is to always have x(i-n) ... x(i-1) available for
  // operator== and save/restore.

  UIntType x[2*n]; 
  int i;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const bool mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::has_fixed_range;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::state_size;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::shift_size;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::mask_bits;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const UIntType mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::parameter_a;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_u;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_s;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const UIntType mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_b;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_t;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const UIntType mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_c;
template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
const int mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::output_l;
#endif

template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
void mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::twist(int block)
{
  const UIntType upper_mask = (~0u) << r;
  const UIntType lower_mask = ~upper_mask;

  if(block == 0) {
    for(int j = n; j < 2*n; j++) {
      UIntType y = (x[j-n] & upper_mask) | (x[j-(n-1)] & lower_mask);
      x[j] = x[j-(n-m)] ^ (y >> 1) ^ (y&1 ? a : 0);
    }
  } else if (block == 1) {
    // split loop to avoid costly modulo operations
    {  // extra scope for MSVC brokenness w.r.t. for scope
      for(int j = 0; j < n-m; j++) {
        UIntType y = (x[j+n] & upper_mask) | (x[j+n+1] & lower_mask);
        x[j] = x[j+n+m] ^ (y >> 1) ^ (y&1 ? a : 0);
      }
    }
    
    for(int j = n-m; j < n-1; j++) {
      UIntType y = (x[j+n] & upper_mask) | (x[j+n+1] & lower_mask);
      x[j] = x[j-(n-m)] ^ (y >> 1) ^ (y&1 ? a : 0);
    }
    // last iteration
    UIntType y = (x[2*n-1] & upper_mask) | (x[0] & lower_mask);
    x[n-1] = x[m-1] ^ (y >> 1) ^ (y&1 ? a : 0);
    i = 0;
  }
}

template<class UIntType, int w, int n, int m, int r, UIntType a, int u,
  int s, UIntType b, int t, UIntType c, int l, UIntType val>
inline typename mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::result_type
mersenne_twister<UIntType,w,n,m,r,a,u,s,b,t,c,l,val>::operator()()
{
  if(i == n)
    twist(0);
  else if(i >= 2*n)
    twist(1);
  // Step 4
  UIntType z = x[i];
  ++i;
  z ^= (z >> u);
  z ^= ((z << s) & b);
  z ^= ((z << t) & c);
  z ^= (z >> l);
  return z;
}

} // namespace random


typedef random::mersenne_twister<uint32_t,32,351,175,19,0xccab8ee7,11,
  7,0x31b6ab00,15,0xffe50000,17, 0xa37d3c92> mt11213b;

// validation by experiment from mt19937.c
typedef random::mersenne_twister<uint32_t,32,624,397,31,0x9908b0df,11,
  7,0x9d2c5680,15,0xefc60000,18, 3346425566U> mt19937;

} // namespace boost

BOOST_RANDOM_PTR_HELPER_SPEC(boost::mt19937)

#endif // BOOST_RANDOM_MERSENNE_TWISTER_HPP
