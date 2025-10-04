//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013.
// (C) Copyright Gennaro Prota 2003 - 2004.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_TRANSFORM_ITERATORS_HPP
#define BOOST_CONTAINER_DETAIL_TRANSFORM_ITERATORS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/iterator.hpp>

namespace boost {
namespace container {

template <class PseudoReference>
struct operator_arrow_proxy
{
   inline operator_arrow_proxy(const PseudoReference &px)
      :  m_value(px)
   {}

   typedef PseudoReference element_type;

   inline PseudoReference* operator->() const { return &m_value; }

   mutable PseudoReference m_value;
};

template <class T>
struct operator_arrow_proxy<T&>
{
   inline operator_arrow_proxy(T &px)
      :  m_value(px)
   {}

   typedef T element_type;

   inline T* operator->() const { return const_cast<T*>(&m_value); }

   T &m_value;
};

template <class Iterator, class UnaryFunction>
class transform_iterator
   : public UnaryFunction
   , public boost::container::iterator
      < typename Iterator::iterator_category
      , typename dtl::remove_reference<typename UnaryFunction::result_type>::type
      , typename Iterator::difference_type
      , operator_arrow_proxy<typename UnaryFunction::result_type>
      , typename UnaryFunction::result_type>
{
   public:
   inline explicit transform_iterator(const Iterator &it, const UnaryFunction &f = UnaryFunction())
      :  UnaryFunction(f), m_it(it)
   {}

   inline explicit transform_iterator()
      :  UnaryFunction(), m_it()
   {}

   //Constructors
   inline transform_iterator& operator++()
   { increment();   return *this;   }

      inline transform_iterator operator++(int)
   {
      transform_iterator result (*this);
      increment();
      return result;
   }

   inline friend bool operator== (const transform_iterator& i, const transform_iterator& i2)
   { return i.equal(i2); }

   inline friend bool operator!= (const transform_iterator& i, const transform_iterator& i2)
   { return !(i == i2); }

/*
   friend bool operator> (const transform_iterator& i, const transform_iterator& i2)
   { return i2 < i; }

   friend bool operator<= (const transform_iterator& i, const transform_iterator& i2)
   { return !(i > i2); }

   friend bool operator>= (const transform_iterator& i, const transform_iterator& i2)
   { return !(i < i2); }
*/
   inline friend typename Iterator::difference_type operator- (const transform_iterator& i, const transform_iterator& i2)
   { return i2.distance_to(i); }

   //Arithmetic
   inline transform_iterator& operator+=(typename Iterator::difference_type off)
   {  this->advance(off); return *this;   }

   inline transform_iterator operator+(typename Iterator::difference_type off) const
   {
      transform_iterator other(*this);
      other.advance(off);
      return other;
   }

   inline friend transform_iterator operator+(typename Iterator::difference_type off, const transform_iterator& right)
   {  return right + off; }

   inline transform_iterator& operator-=(typename Iterator::difference_type off)
   {  this->advance(-off); return *this;   }

   inline transform_iterator operator-(typename Iterator::difference_type off) const
   {  return *this + (-off);  }

   inline typename UnaryFunction::result_type operator*() const
   { return dereference(); }

   inline operator_arrow_proxy<typename UnaryFunction::result_type>
      operator->() const
   { return operator_arrow_proxy<typename UnaryFunction::result_type>(dereference());  }

   inline Iterator & base()
   {  return m_it;   }

   inline const Iterator & base() const
   {  return m_it;   }

   private:
   Iterator m_it;

   inline void increment()
   { ++m_it; }

   inline void decrement()
   { --m_it; }

   inline bool equal(const transform_iterator &other) const
   {  return m_it == other.m_it;   }

   inline bool less(const transform_iterator &other) const
   {  return other.m_it < m_it;   }

   inline typename UnaryFunction::result_type dereference() const
   { return UnaryFunction::operator()(*m_it); }

   inline void advance(typename Iterator::difference_type n)
   {  boost::container::iterator_advance(m_it, n); }

   inline typename Iterator::difference_type distance_to(const transform_iterator &other)const
   {  return boost::container::iterator_distance(other.m_it, m_it); }
};

template <class Iterator, class UnaryFunc>
inline transform_iterator<Iterator, UnaryFunc>
make_transform_iterator(Iterator it, UnaryFunc fun)
{
   return transform_iterator<Iterator, UnaryFunc>(it, fun);
}

}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_TRANSFORM_ITERATORS_HPP
