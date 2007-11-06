
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_HAS_NOTHROW_COPY_HPP_INCLUDED
#define BOOST_TT_HAS_NOTHROW_COPY_HPP_INCLUDED

#include <boost/type_traits/has_trivial_copy.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

namespace detail{

template <class T>
struct has_nothrow_copy_imp{
   BOOST_STATIC_CONSTANT(bool, value = 
      (::boost::type_traits::ice_or<
         ::boost::has_trivial_copy<T>::value,
         BOOST_HAS_NOTHROW_COPY(T)
      >::value));
};

}

BOOST_TT_AUX_BOOL_TRAIT_DEF1(has_nothrow_copy,T,::boost::detail::has_nothrow_copy_imp<T>::value)

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_HAS_NOTHROW_COPY_HPP_INCLUDED
