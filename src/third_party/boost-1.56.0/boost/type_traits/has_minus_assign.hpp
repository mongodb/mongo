//  (C) Copyright 2009-2011 Frederic Bron.
//
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_HAS_MINUS_ASSIGN_HPP_INCLUDED
#define BOOST_TT_HAS_MINUS_ASSIGN_HPP_INCLUDED

#define BOOST_TT_TRAIT_NAME has_minus_assign
#define BOOST_TT_TRAIT_OP -=
#define BOOST_TT_FORBIDDEN_IF\
   ::boost::type_traits::ice_or<\
      /* Lhs==pointer and Rhs==fundamental and Rhs!=integral */\
      ::boost::type_traits::ice_and<\
         ::boost::is_pointer< Lhs_noref >::value,\
         ::boost::is_fundamental< Rhs_nocv >::value,\
         ::boost::type_traits::ice_not< ::boost::is_integral< Rhs_noref >::value >::value\
      >::value,\
      /* Lhs==void* and Rhs==fundamental */\
      ::boost::type_traits::ice_and<\
         ::boost::is_pointer< Lhs_noref >::value,\
         ::boost::is_void< Lhs_noptr >::value,\
         ::boost::is_fundamental< Rhs_nocv >::value\
      >::value,\
      /* Rhs==void* and Lhs==fundamental */\
      ::boost::type_traits::ice_and<\
         ::boost::is_pointer< Rhs_noref >::value,\
         ::boost::is_void< Rhs_noptr >::value,\
         ::boost::is_fundamental< Lhs_nocv >::value\
      >::value,\
      /* Lhs=fundamental and Rhs=pointer */\
      ::boost::type_traits::ice_and<\
         ::boost::is_fundamental< Lhs_nocv >::value,\
         ::boost::is_pointer< Rhs_noref >::value\
      >::value,\
      /* Lhs==pointer and Rhs==pointer */\
      ::boost::type_traits::ice_and<\
         ::boost::is_pointer< Lhs_noref >::value,\
         ::boost::is_pointer< Rhs_noref >::value\
      >::value,\
      /* (Lhs==fundamental or Lhs==pointer) and (Rhs==fundamental or Rhs==pointer) and (Lhs==const) */\
      ::boost::type_traits::ice_and<\
         ::boost::type_traits::ice_or<\
            ::boost::is_fundamental< Lhs_nocv >::value,\
            ::boost::is_pointer< Lhs_noref >::value\
         >::value,\
         ::boost::type_traits::ice_or<\
            ::boost::is_fundamental< Rhs_nocv >::value,\
            ::boost::is_pointer< Rhs_noref >::value\
         >::value,\
         ::boost::is_const< Lhs_noref >::value\
      >::value\
   >::value


#include <boost/type_traits/detail/has_binary_operator.hpp>

#undef BOOST_TT_TRAIT_NAME
#undef BOOST_TT_TRAIT_OP
#undef BOOST_TT_FORBIDDEN_IF

#endif
