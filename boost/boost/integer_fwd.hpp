//  Boost integer_fwd.hpp header file  ---------------------------------------//

//  (C) Copyright Dave Abrahams and Daryle Walker 2001. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/integer for documentation.

#ifndef BOOST_INTEGER_FWD_HPP
#define BOOST_INTEGER_FWD_HPP

#include <climits>  // for UCHAR_MAX, etc.
#include <cstddef>  // for std::size_t

#include <boost/config.hpp>  // for BOOST_NO_INTRINSIC_WCHAR_T
#include <boost/limits.hpp>  // for std::numeric_limits


namespace boost
{


//  From <boost/cstdint.hpp>  ------------------------------------------------//

// Only has typedefs or using statements, with #conditionals


//  From <boost/integer_traits.hpp>  -----------------------------------------//

template < class T >
    class integer_traits;

template <  >
    class integer_traits< bool >;

template <  >
    class integer_traits< char >;

template <  >
    class integer_traits< signed char >;

template <  >
    class integer_traits< unsigned char >;

#ifndef BOOST_NO_INTRINSIC_WCHAR_T
template <  >
    class integer_traits< wchar_t >;
#endif

template <  >
    class integer_traits< short >;

template <  >
    class integer_traits< unsigned short >;

template <  >
    class integer_traits< int >;

template <  >
    class integer_traits< unsigned int >;

template <  >
    class integer_traits< long >;

template <  >
    class integer_traits< unsigned long >;

#ifdef ULLONG_MAX
template <  >
    class integer_traits<  ::boost::long_long_type>;

template <  >
    class integer_traits<  ::boost::ulong_long_type >;
#endif


//  From <boost/integer.hpp>  ------------------------------------------------//

template < typename LeastInt >
    struct int_fast_t;

template< int Bits >
    struct int_t;

template< int Bits >
    struct uint_t;

template< long MaxValue >
    struct int_max_value_t;

template< long MinValue >
    struct int_min_value_t;

template< unsigned long Value >
    struct uint_value_t;


//  From <boost/integer/integer_mask.hpp>  -----------------------------------//

template < std::size_t Bit >
    struct high_bit_mask_t;

template < std::size_t Bits >
    struct low_bits_mask_t;

template <  >
    struct low_bits_mask_t< ::std::numeric_limits<unsigned char>::digits >;

#if USHRT_MAX > UCHAR_MAX
template <  >
    struct low_bits_mask_t< ::std::numeric_limits<unsigned short>::digits >;
#endif

#if UINT_MAX > USHRT_MAX
template <  >
    struct low_bits_mask_t< ::std::numeric_limits<unsigned int>::digits >;
#endif

#if ULONG_MAX > UINT_MAX
template <  >
    struct low_bits_mask_t< ::std::numeric_limits<unsigned long>::digits >;
#endif


//  From <boost/integer/static_log2.hpp>  ------------------------------------//

template < unsigned long Value >
    struct static_log2;

template <  >
    struct static_log2< 0ul >;


//  From <boost/integer/static_min_max.hpp>  ---------------------------------//

template < long Value1, long Value2 >
    struct static_signed_min;

template < long Value1, long Value2 >
    struct static_signed_max;

template < unsigned long Value1, unsigned long Value2 >
    struct static_unsigned_min;

template < unsigned long Value1, unsigned long Value2 >
    struct static_unsigned_max;


}  // namespace boost


#endif  // BOOST_INTEGER_FWD_HPP
