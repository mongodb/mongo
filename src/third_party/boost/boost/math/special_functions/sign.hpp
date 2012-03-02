//  (C) Copyright John Maddock 2006.
//  (C) Copyright Johan Rade 2006.
//  (C) Copyright Paul A. Bristow 2011 (added changesign).

//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_TOOLS_SIGN_HPP
#define BOOST_MATH_TOOLS_SIGN_HPP

#ifdef _MSC_VER
#pragma once
#endif

#include <boost/math/tools/config.hpp>
#include <boost/math/special_functions/math_fwd.hpp>
#include <boost/math/special_functions/detail/fp_traits.hpp>

namespace boost{ namespace math{ 

namespace detail {

  // signbit

#ifdef BOOST_MATH_USE_STD_FPCLASSIFY
    template<class T> 
    inline int signbit_impl(T x, native_tag const&)
    {
        return (std::signbit)(x);
    }
#endif

    template<class T> 
    inline int signbit_impl(T x, generic_tag<true> const&)
    {
        return x < 0;
    }

    template<class T> 
    inline int signbit_impl(T x, generic_tag<false> const&)
    {
        return x < 0;
    }

    template<class T> 
    inline int signbit_impl(T x, ieee_copy_all_bits_tag const&)
    {
        typedef BOOST_DEDUCED_TYPENAME fp_traits<T>::type traits;

        BOOST_DEDUCED_TYPENAME traits::bits a;
        traits::get_bits(x,a);
        return a & traits::sign ? 1 : 0;
    }

    template<class T> 
    inline int signbit_impl(T x, ieee_copy_leading_bits_tag const&)
    {
        typedef BOOST_DEDUCED_TYPENAME fp_traits<T>::type traits;

        BOOST_DEDUCED_TYPENAME traits::bits a;
        traits::get_bits(x,a);

        return a & traits::sign ? 1 : 0;
    }

    // Changesign

    template<class T>
    inline T (changesign_impl)(T x, generic_tag<true> const&)
    {
        return -x;
    }

    template<class T>
    inline T (changesign_impl)(T x, generic_tag<false> const&)
    {
        return -x;
    }


    template<class T>
    inline T changesign_impl(T x, ieee_copy_all_bits_tag const&)
    {
        typedef BOOST_DEDUCED_TYPENAME fp_traits<T>::sign_change_type traits;

        BOOST_DEDUCED_TYPENAME traits::bits a;
        traits::get_bits(x,a);
        a ^= traits::sign;
        traits::set_bits(x,a);
        return x;
    }

    template<class T>
    inline T (changesign_impl)(T x, ieee_copy_leading_bits_tag const&)
    {
        typedef BOOST_DEDUCED_TYPENAME fp_traits<T>::sign_change_type traits;

        BOOST_DEDUCED_TYPENAME traits::bits a;
        traits::get_bits(x,a);
        a ^= traits::sign;
        traits::set_bits(x,a);
        return x;
    }


}   // namespace detail

template<class T> int (signbit)(T x)
{ 
   typedef typename detail::fp_traits<T>::type traits;
   typedef typename traits::method method;
   typedef typename boost::is_floating_point<T>::type fp_tag;
   return detail::signbit_impl(x, method());
}

template <class T>
inline int sign BOOST_NO_MACRO_EXPAND(const T& z)
{
   return (z == 0) ? 0 : (boost::math::signbit)(z) ? -1 : 1;
}

template<class T> T (changesign)(const T& x)
{ //!< \brief return unchanged binary pattern of x, except for change of sign bit. 
   typedef typename detail::fp_traits<T>::sign_change_type traits;
   typedef typename traits::method method;
   typedef typename boost::is_floating_point<T>::type fp_tag;

   return detail::changesign_impl(x, method());
}

template <class T>
inline T copysign BOOST_NO_MACRO_EXPAND(const T& x, const T& y)
{
   BOOST_MATH_STD_USING
   return (boost::math::signbit)(x) != (boost::math::signbit)(y) ? (boost::math::changesign)(x) : x;
}

} // namespace math
} // namespace boost


#endif // BOOST_MATH_TOOLS_SIGN_HPP


