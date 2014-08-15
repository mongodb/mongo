
//  (C) Copyright John Maddock 2005.  
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.


#ifndef BOOST_TT_IS_MEMBER_OBJECT_POINTER_HPP_INCLUDED
#define BOOST_TT_IS_MEMBER_OBJECT_POINTER_HPP_INCLUDED

#include <boost/type_traits/config.hpp>
#include <boost/type_traits/is_member_pointer.hpp>
#include <boost/type_traits/is_member_function_pointer.hpp>
#include <boost/type_traits/detail/ice_and.hpp>
#include <boost/type_traits/detail/ice_not.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

namespace detail{

template <typename T>
struct is_member_object_pointer_impl
{
   BOOST_STATIC_CONSTANT(
      bool, value = (::boost::type_traits::ice_and<
         ::boost::is_member_pointer<T>::value,
         ::boost::type_traits::ice_not<
            ::boost::is_member_function_pointer<T>::value
         >::value
      >::value ));
};

} // namespace detail

BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_member_object_pointer,T,::boost::detail::is_member_object_pointer_impl<T>::value)

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_MEMBER_FUNCTION_POINTER_HPP_INCLUDED
