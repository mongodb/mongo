
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_HAS_NOTHROW_DESTRUCTOR_HPP_INCLUDED
#define BOOST_TT_HAS_NOTHROW_DESTRUCTOR_HPP_INCLUDED

#include <boost/type_traits/has_trivial_destructor.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

BOOST_TT_AUX_BOOL_TRAIT_DEF1(has_nothrow_destructor,T,::boost::has_trivial_destructor<T>::value)

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_HAS_NOTHROW_DESTRUCTOR_HPP_INCLUDED
