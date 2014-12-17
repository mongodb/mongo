//  (C) Copyright John Maddock 2007. 
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_IS_COMPLEX_HPP
#define BOOST_TT_IS_COMPLEX_HPP

#include <boost/type_traits/is_convertible.hpp>
#include <complex>
// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>


namespace boost {
namespace detail{

struct is_convertible_from_tester
{
   template <class T>
   is_convertible_from_tester(const std::complex<T>&);
};

}

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_complex,T,(::boost::is_convertible<T, boost::detail::is_convertible_from_tester>::value))

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif //BOOST_TT_IS_COMPLEX_HPP
