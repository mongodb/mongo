/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP
#define BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/iterator.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template <class PseudoReference>
struct operator_arrow_proxy
{
   inline operator_arrow_proxy(const PseudoReference &px)
      :  m_value(px)
   {}

   inline PseudoReference* operator->() const { return &m_value; }
   // This function is needed for MWCW and BCC, which won't call operator->
   // again automatically per 13.3.1.2 para 8
//   operator T*() const { return &m_value; }
   mutable PseudoReference m_value;
};

template <class T>
struct operator_arrow_proxy<T&>
{
   inline operator_arrow_proxy(T &px)
      :  m_value(px)
   {}

   inline T* operator->() const { return &m_value; }
   // This function is needed for MWCW and BCC, which won't call operator->
   // again automatically per 13.3.1.2 para 8
//   operator T*() const { return &m_value; }
   T &m_value;
};

template <class Iterator, class UnaryFunction>
class transform_iterator
{
   public:
   typedef typename Iterator::iterator_category                                           iterator_category;
   typedef typename detail::remove_reference<typename UnaryFunction::result_type>::type   value_type;
   typedef typename Iterator::difference_type                                             difference_type;
   typedef operator_arrow_proxy<typename UnaryFunction::result_type>                      pointer;
   typedef typename UnaryFunction::result_type                                            reference;
   
   explicit transform_iterator(const Iterator &it, const UnaryFunction &f = UnaryFunction())
      :  members_(it, f)
   {}

   explicit transform_iterator()
      :  members_()
   {}

   inline Iterator get_it() const
   {  return members_.m_it;   }

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

   inline friend typename Iterator::difference_type operator- (const transform_iterator& i, const transform_iterator& i2)
   { return i2.distance_to(i); }

   //Arithmetic
   transform_iterator& operator+=(typename Iterator::difference_type off)
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

   private:
   struct members
      :  UnaryFunction
   {
      inline members(const Iterator &it, const UnaryFunction &f)
         :  UnaryFunction(f), m_it(it)
      {}

      inline members()
      {}

      Iterator m_it;
   } members_;


   inline void increment()
   { ++members_.m_it; }

   inline void decrement()
   { --members_.m_it; }

   inline bool equal(const transform_iterator &other) const
   {  return members_.m_it == other.members_.m_it;   }

   inline bool less(const transform_iterator &other) const
   {  return other.members_.m_it < members_.m_it;   }

   typename UnaryFunction::result_type dereference() const
   { return members_(*members_.m_it); }

   void advance(typename Iterator::difference_type n)
   {  boost::intrusive::iterator_advance(members_.m_it, n); }

   typename Iterator::difference_type distance_to(const transform_iterator &other)const
   {  return boost::intrusive::iterator_distance(other.members_.m_it, members_.m_it); }
};

} //namespace detail
} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP
