//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2015-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP
#define BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>

namespace boost{
namespace intrusive{

template<class Key, class T, class KeyCompare, class KeyOfValue>
struct tree_value_compare
   :  public boost::intrusive::detail::ebo_functor_holder<KeyCompare>
{
   typedef boost::intrusive::detail::ebo_functor_holder<KeyCompare> base_t;
   typedef T            value_type;
   typedef KeyCompare   key_compare;
   typedef KeyOfValue   key_of_value;
   typedef Key          key_type;


   tree_value_compare()
      :  base_t()
   {}

   explicit tree_value_compare(const key_compare &kcomp)
      :  base_t(kcomp)
   {}

   tree_value_compare (const tree_value_compare &x)
      :  base_t(x.base_t::get())
   {}

   tree_value_compare &operator=(const tree_value_compare &x)
   {  this->base_t::get() = x.base_t::get();   return *this;  }

   tree_value_compare &operator=(const key_compare &x)
   {  this->base_t::get() = x;   return *this;  }

   const key_compare &key_comp() const
   {  return static_cast<const key_compare &>(*this);  }

   key_compare &key_comp()
   {  return static_cast<key_compare &>(*this);  }

   template<class U>
   struct is_key
      : boost::intrusive::detail::is_same<const U, const key_type>
   {};

   template<class U>
   const key_type & key_forward
      (const U &key, typename boost::intrusive::detail::enable_if<is_key<U> >::type* = 0) const
   {  return key; }

   template<class U>
   const key_type & key_forward
      (const U &key, typename boost::intrusive::detail::disable_if<is_key<U> >::type* = 0) const
   {  return KeyOfValue()(key);  }

   template<class KeyType, class KeyType2>
   bool operator()(const KeyType &key1, const KeyType2 &key2) const
   {  return key_compare::operator()(this->key_forward(key1), this->key_forward(key2));  }

   template<class KeyType, class KeyType2>
   bool operator()(const KeyType &key1, const KeyType2 &key2)
   {  return key_compare::operator()(this->key_forward(key1), this->key_forward(key2));  }
};

}  //namespace intrusive{
}  //namespace boost{

#endif   //#ifdef BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP
