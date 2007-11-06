// Copyright (C) 2003, Fernando Luis Cacciola Carballal.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/lib/optional for documentation.
//
// You are welcome to contact the author at:
//  fernando_cacciola@hotmail.com
//
#ifndef BOOST_UTILITY_TYPED_INPLACE_FACTORY_25AGO2003_HPP
#define BOOST_UTILITY_TYPED_INPLACE_FACTORY_25AGO2003_HPP

#include <boost/utility/detail/in_place_factory_prefix.hpp>

namespace boost {

class typed_in_place_factory_base {} ;

#define BOOST_DEFINE_TYPED_INPLACE_FACTORY_CLASS(z,n,_) \
template< class T, BOOST_PP_ENUM_PARAMS(BOOST_PP_INC(n),class A) > \
class BOOST_PP_CAT(typed_in_place_factory, BOOST_PP_INC(n) ) : public typed_in_place_factory_base \
{ \
public: \
\
  typedef T value_type ; \
\
  BOOST_PP_CAT(typed_in_place_factory, BOOST_PP_INC(n) ) ( BOOST_PP_ENUM_BINARY_PARAMS(BOOST_PP_INC(n),A,const& a) ) \
    : \
    BOOST_PP_ENUM( BOOST_PP_INC(n), BOOST_DEFINE_INPLACE_FACTORY_CLASS_MEMBER_INIT, _ ) \
  {} \
\
  void apply ( void* address ) const \
  { \
    new ( address ) T ( BOOST_PP_ENUM_PARAMS( BOOST_PP_INC(n), m_a ) ) ; \
  } \
\
  BOOST_PP_REPEAT( BOOST_PP_INC(n), BOOST_DEFINE_INPLACE_FACTORY_CLASS_MEMBER_DECL, _) \
} ; \
\
template< class T, BOOST_PP_ENUM_PARAMS(BOOST_PP_INC(n),class A) > \
BOOST_PP_CAT(typed_in_place_factory, BOOST_PP_INC(n) ) < T , BOOST_PP_ENUM_PARAMS( BOOST_PP_INC(n), A ) > \
in_place ( BOOST_PP_ENUM_BINARY_PARAMS(BOOST_PP_INC(n),A, const& a) ) \
{ \
  return BOOST_PP_CAT(typed_in_place_factory, BOOST_PP_INC(n) ) < T, BOOST_PP_ENUM_PARAMS( BOOST_PP_INC(n), A ) > \
           ( BOOST_PP_ENUM_PARAMS( BOOST_PP_INC(n), a ) ) ; \
} ; \

BOOST_PP_REPEAT( BOOST_MAX_INPLACE_FACTORY_ARITY, BOOST_DEFINE_TYPED_INPLACE_FACTORY_CLASS, BOOST_PP_EMPTY() )

} // namespace boost

#include <boost/utility/detail/in_place_factory_suffix.hpp>

#endif

