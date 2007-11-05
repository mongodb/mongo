// Copyright 2002, Fernando Luis Cacciola Carballal.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// 21 Ago 2002 (Created) Fernando Cacciola
//
#ifndef BOOST_UTILITY_VALUE_INIT_21AGO2002_HPP
#define BOOST_UTILITY_VALUE_INIT_21AGO2002_HPP

#include "boost/detail/select_type.hpp"
#include "boost/type_traits/cv_traits.hpp"

namespace boost {

namespace vinit_detail {

template<class T>
class const_T_base
{
  protected :

   const_T_base() : x() {}

   T x ;
} ;

template<class T>
struct non_const_T_base
{
  protected :

   non_const_T_base() : x() {}

   mutable T x ;
} ;

template<class T>
struct select_base
{
  typedef typename
    boost::detail::if_true< ::boost::is_const<T>::value >
      ::template then< const_T_base<T>, non_const_T_base<T> >::type type ;
} ;

} // namespace vinit_detail

template<class T>
class value_initialized : private vinit_detail::select_base<T>::type
{
  public :

    value_initialized() {}

    operator T&() const { return this->x ; }

    T& data() const { return this->x ; }

} ;

template<class T>
T const& get ( value_initialized<T> const& x )
{
  return x.data() ;
}
template<class T>
T& get ( value_initialized<T>& x )
{
  return x.data() ;
}

} // namespace boost


#endif

