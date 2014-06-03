//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_VARIADIC_TEMPLATES_TOOLS_HPP
#define BOOST_CONTAINER_DETAIL_VARIADIC_TEMPLATES_TOOLS_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <cstddef>   //std::size_t

namespace boost {
namespace container { 
namespace container_detail {

template<typename... Values>
class tuple;

template<> class tuple<>
{};

template<typename Head, typename... Tail>
class tuple<Head, Tail...>
   : private tuple<Tail...>
{
   typedef tuple<Tail...> inherited;

   public:
   tuple() { }

   // implicit copy-constructor is okay
   // Construct tuple from separate arguments.
   tuple(typename add_const_reference<Head>::type v,
         typename add_const_reference<Tail>::type... vtail)
   : inherited(vtail...), m_head(v)
   {}

   // Construct tuple from another tuple.
   template<typename... VValues>
   tuple(const tuple<VValues...>& other)
      : m_head(other.head()), inherited(other.tail())
   {}

   template<typename... VValues>
   tuple& operator=(const tuple<VValues...>& other)
   {
      m_head = other.head();
      tail() = other.tail();
      return this;
   }

   typename add_reference<Head>::type head()             {  return m_head; }
   typename add_reference<const Head>::type head() const {  return m_head; }

   inherited& tail()             { return *this; }
   const inherited& tail() const { return *this; }

   protected:
   Head m_head;
};


template<typename... Values>
tuple<Values&&...> tie_forward(Values&&... values)
{ return tuple<Values&&...>(values...); }

template<int I, typename Tuple>
struct tuple_element;

template<int I, typename Head, typename... Tail>
struct tuple_element<I, tuple<Head, Tail...> >
{
   typedef typename tuple_element<I-1, tuple<Tail...> >::type type;
};

template<typename Head, typename... Tail>
struct tuple_element<0, tuple<Head, Tail...> >
{
   typedef Head type;
};

template<int I, typename Tuple>
class get_impl;

template<int I, typename Head, typename... Values>
class get_impl<I, tuple<Head, Values...> >
{
   typedef typename tuple_element<I-1, tuple<Values...> >::type   Element;
   typedef get_impl<I-1, tuple<Values...> >                       Next;

   public:
   typedef typename add_reference<Element>::type                  type;
   typedef typename add_const_reference<Element>::type            const_type;
   static type get(tuple<Head, Values...>& t)              { return Next::get(t.tail()); }
   static const_type get(const tuple<Head, Values...>& t)  { return Next::get(t.tail()); }
};

template<typename Head, typename... Values>
class get_impl<0, tuple<Head, Values...> >
{
   public:
   typedef typename add_reference<Head>::type         type;
   typedef typename add_const_reference<Head>::type   const_type;
   static type       get(tuple<Head, Values...>& t)      { return t.head(); }
   static const_type get(const tuple<Head, Values...>& t){ return t.head(); }
};

template<int I, typename... Values>
typename get_impl<I, tuple<Values...> >::type get(tuple<Values...>& t)
{  return get_impl<I, tuple<Values...> >::get(t);  }

template<int I, typename... Values>
typename get_impl<I, tuple<Values...> >::const_type get(const tuple<Values...>& t)
{  return get_impl<I, tuple<Values...> >::get(t);  }

////////////////////////////////////////////////////
// Builds an index_tuple<0, 1, 2, ..., Num-1>, that will
// be used to "unpack" into comma-separated values
// in a function call.
////////////////////////////////////////////////////

template<int... Indexes>
struct index_tuple{};

template<std::size_t Num, typename Tuple = index_tuple<> >
struct build_number_seq;

template<std::size_t Num, int... Indexes> 
struct build_number_seq<Num, index_tuple<Indexes...> >
   : build_number_seq<Num - 1, index_tuple<Indexes..., sizeof...(Indexes)> >
{};

template<int... Indexes>
struct build_number_seq<0, index_tuple<Indexes...> >
{  typedef index_tuple<Indexes...> type;  };


}}}   //namespace boost { namespace container { namespace container_detail {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_VARIADIC_TEMPLATES_TOOLS_HPP
