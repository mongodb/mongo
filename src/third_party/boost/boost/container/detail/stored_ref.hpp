//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_STORED_REF_HPP
#define BOOST_CONTAINER_DETAIL_STORED_REF_HPP

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>

#ifndef BOOST_NO_RVALUE_REFERENCES

namespace boost{
namespace container{
namespace container_detail{

template<class T>
struct stored_ref
{

   static T && forward(T &t)
   #ifdef BOOST_MOVE_OLD_RVALUE_REF_BINDING_RULES
   { return t; }
   #else
   { return boost::move(t); }
   #endif
};

template<class T>
struct stored_ref<const T>
{
   static const T && forward(const T &t)
   #ifdef BOOST_MOVE_OLD_RVALUE_REF_BINDING_RULES
   { return t; }
   #else
   { return static_cast<const T&&>(t); }
   #endif
};

template<class T>
struct stored_ref<T&&>
{
   static T && forward(T &t)
   #ifdef BOOST_MOVE_OLD_RVALUE_REF_BINDING_RULES
   { return t; }
   #else
   { return boost::move(t); }
   #endif
};

template<class T>
struct stored_ref<const T&&>
{
   static const T && forward(const T &t)
   #ifdef BOOST_MOVE_OLD_RVALUE_REF_BINDING_RULES
   { return t; }
   #else
   { return static_cast<const T &&>(t); }
   #endif
};

template<class T>
struct stored_ref<const T&>
{
   static const T & forward(const T &t)
   {  return t; }
};

template<class T>
struct stored_ref<T&>
{
   static T & forward(T &t)
   {  return t; }
};

}  //namespace container_detail{
}  //namespace container{
}  //namespace boost{

#else
#error "This header can be included only for compiler with rvalue references"
#endif   //BOOST_NO_RVALUE_REFERENCES

#include <boost/container/detail/config_end.hpp>

#endif   //BOOST_CONTAINER_DETAIL_STORED_REF_HPP
