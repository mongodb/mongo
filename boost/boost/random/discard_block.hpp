/* boost random/discard_block.hpp header file
 *
 * Copyright Jens Maurer 2002
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: discard_block.hpp,v 1.12 2005/05/21 15:57:00 dgregor Exp $
 *
 * Revision history
 *  2001-03-02  created
 */

#ifndef BOOST_RANDOM_DISCARD_BLOCK_HPP
#define BOOST_RANDOM_DISCARD_BLOCK_HPP

#include <iostream>
#include <boost/config.hpp>
#include <boost/limits.hpp>
#include <boost/static_assert.hpp>


namespace boost {
namespace random {

template<class UniformRandomNumberGenerator, unsigned int p, unsigned int r>
class discard_block
{
public:
  typedef UniformRandomNumberGenerator base_type;
  typedef typename base_type::result_type result_type;

  BOOST_STATIC_CONSTANT(bool, has_fixed_range = false);
  BOOST_STATIC_CONSTANT(unsigned int, total_block = p);
  BOOST_STATIC_CONSTANT(unsigned int, returned_block = r);

#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
  BOOST_STATIC_ASSERT(total_block >= returned_block);
#endif

  discard_block() : _rng(), _n(0) { }
  explicit discard_block(const base_type & rng) : _rng(rng), _n(0) { }
  template<class It> discard_block(It& first, It last)
    : _rng(first, last), _n(0) { }
  void seed() { _rng.seed(); _n = 0; }
  template<class T> void seed(T s) { _rng.seed(s); _n = 0; }
  template<class It> void seed(It& first, It last)
  { _n = 0; _rng.seed(first, last); }

  const base_type& base() const { return _rng; }

  result_type operator()()
  {
    if(_n >= returned_block) {
      // discard values of random number generator
      for( ; _n < total_block; ++_n)
        _rng();
      _n = 0;
    }
    ++_n;
    return _rng();
  }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (_rng.min)(); }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (_rng.max)(); }
  static bool validation(result_type x) { return true; }  // dummy

#ifndef BOOST_NO_OPERATORS_IN_NAMESPACE

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
  template<class CharT, class Traits>
  friend std::basic_ostream<CharT,Traits>&
  operator<<(std::basic_ostream<CharT,Traits>& os, const discard_block& s)
  {
    os << s._rng << " " << s._n << " ";
    return os;
  }

  template<class CharT, class Traits>
  friend std::basic_istream<CharT,Traits>&
  operator>>(std::basic_istream<CharT,Traits>& is, discard_block& s)
  {
    is >> s._rng >> std::ws >> s._n >> std::ws;
    return is;
  }
#endif

  friend bool operator==(const discard_block& x, const discard_block& y)
  { return x._rng == y._rng && x._n == y._n; }
  friend bool operator!=(const discard_block& x, const discard_block& y)
  { return !(x == y); }
#else
  // Use a member function; Streamable concept not supported.
  bool operator==(const discard_block& rhs) const
  { return _rng == rhs._rng && _n == rhs._n; }
  bool operator!=(const discard_block& rhs) const
  { return !(*this == rhs); }
#endif

private:
  base_type _rng;
  unsigned int _n;
};

#ifndef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
//  A definition is required even for integral static constants
template<class UniformRandomNumberGenerator, unsigned int p, unsigned int r>
const bool discard_block<UniformRandomNumberGenerator, p, r>::has_fixed_range;
template<class UniformRandomNumberGenerator, unsigned int p, unsigned int r>
const unsigned int discard_block<UniformRandomNumberGenerator, p, r>::total_block;
template<class UniformRandomNumberGenerator, unsigned int p, unsigned int r>
const unsigned int discard_block<UniformRandomNumberGenerator, p, r>::returned_block;
#endif

} // namespace random

} // namespace boost

#endif // BOOST_RANDOM_DISCARD_BLOCK_HPP
