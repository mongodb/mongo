//  Boost integer/integer_mask.hpp header file  ------------------------------//

//  (C) Copyright Daryle Walker 2001.
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history. 

#ifndef BOOST_INTEGER_INTEGER_MASK_HPP
#define BOOST_INTEGER_INTEGER_MASK_HPP

#include <boost/integer_fwd.hpp>  // self include

#include <boost/config.hpp>   // for BOOST_STATIC_CONSTANT
#include <boost/integer.hpp>  // for boost::uint_t

#include <climits>  // for UCHAR_MAX, etc.
#include <cstddef>  // for std::size_t

#include <boost/limits.hpp>  // for std::numeric_limits


namespace boost
{


//  Specified single-bit mask class declaration  -----------------------------//
//  (Lowest bit starts counting at 0.)

template < std::size_t Bit >
struct high_bit_mask_t
{
    typedef typename uint_t<(Bit + 1)>::least  least;
    typedef typename uint_t<(Bit + 1)>::fast   fast;

    BOOST_STATIC_CONSTANT( least, high_bit = (least( 1u ) << Bit) );
    BOOST_STATIC_CONSTANT( fast, high_bit_fast = (fast( 1u ) << Bit) );

    BOOST_STATIC_CONSTANT( std::size_t, bit_position = Bit );

};  // boost::high_bit_mask_t


//  Specified bit-block mask class declaration  ------------------------------//
//  Makes masks for the lowest N bits
//  (Specializations are needed when N fills up a type.)

template < std::size_t Bits >
struct low_bits_mask_t
{
    typedef typename uint_t<Bits>::least  least;
    typedef typename uint_t<Bits>::fast   fast;

    BOOST_STATIC_CONSTANT( least, sig_bits = (~( ~(least( 0u )) << Bits )) );
    BOOST_STATIC_CONSTANT( fast, sig_bits_fast = fast(sig_bits) );

    BOOST_STATIC_CONSTANT( std::size_t, bit_count = Bits );

};  // boost::low_bits_mask_t


#define BOOST_LOW_BITS_MASK_SPECIALIZE( Type )                                  \
  template <  >  struct low_bits_mask_t< std::numeric_limits<Type>::digits >  { \
      typedef std::numeric_limits<Type>           limits_type;                  \
      typedef uint_t<limits_type::digits>::least  least;                        \
      typedef uint_t<limits_type::digits>::fast   fast;                         \
      BOOST_STATIC_CONSTANT( least, sig_bits = (~( least(0u) )) );              \
      BOOST_STATIC_CONSTANT( fast, sig_bits_fast = fast(sig_bits) );            \
      BOOST_STATIC_CONSTANT( std::size_t, bit_count = limits_type::digits );    \
  }

BOOST_LOW_BITS_MASK_SPECIALIZE( unsigned char );

#if USHRT_MAX > UCHAR_MAX
BOOST_LOW_BITS_MASK_SPECIALIZE( unsigned short );
#endif

#if UINT_MAX > USHRT_MAX
BOOST_LOW_BITS_MASK_SPECIALIZE( unsigned int );
#endif

#if ULONG_MAX > UINT_MAX
BOOST_LOW_BITS_MASK_SPECIALIZE( unsigned long );
#endif

#undef BOOST_LOW_BITS_MASK_SPECIALIZE


}  // namespace boost


#endif  // BOOST_INTEGER_INTEGER_MASK_HPP
