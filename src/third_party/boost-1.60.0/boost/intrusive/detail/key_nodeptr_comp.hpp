/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP
#define BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template < class KeyTypeKeyCompare
         , class ValueTraits
         , class KeyOfValue = void
         >
struct key_nodeptr_comp
   //Use public inheritance to avoid MSVC bugs with closures
   :  public ebo_functor_holder<KeyTypeKeyCompare>
{
   typedef ValueTraits                             value_traits;
   typedef typename value_traits::value_type       value_type;
   typedef typename value_traits::node_ptr         node_ptr;
   typedef typename value_traits::const_node_ptr   const_node_ptr;
   typedef ebo_functor_holder<KeyTypeKeyCompare>   base_t;
   typedef typename detail::if_c
            < detail::is_same<KeyOfValue, void>::value
            , detail::identity<value_type>
            , KeyOfValue
            >::type                                key_of_value;
   typedef typename key_of_value::type         key_type;

   key_nodeptr_comp(KeyTypeKeyCompare kcomp, const ValueTraits *traits)
      :  base_t(kcomp), traits_(traits)
   {}

   template<class T>
   struct is_node_ptr
   {
      static const bool value = is_same<T, const_node_ptr>::value || is_same<T, node_ptr>::value;
   };

   //key_forward
   template<class T>
   typename enable_if<is_node_ptr<T>, const key_type &>::type key_forward(const T &node) const
   {  return key_of_value()(*traits_->to_value_ptr(node));  }

   template<class T>
   #if defined(BOOST_MOVE_HELPERS_RETURN_SFINAE_BROKEN)
   const T &key_forward (const T &key, typename disable_if<is_node_ptr<T> >::type* =0) const
   #else
   typename disable_if<is_node_ptr<T>, const T &>::type key_forward(const T &key) const
   #endif
   {  return key;  }

   //operator() 1 arg
   template<class KeyType>
   bool operator()(const KeyType &key1) const
   {  return base_t::get()(this->key_forward(key1));  }

   template<class KeyType>
   bool operator()(const KeyType &key1)
   {  return base_t::get()(this->key_forward(key1));  }

   //operator() 2 arg
   template<class KeyType, class KeyType2>
   bool operator()(const KeyType &key1, const KeyType2 &key2) const
   {  return base_t::get()(this->key_forward(key1), this->key_forward(key2));  }

   template<class KeyType, class KeyType2>
   bool operator()(const KeyType &key1, const KeyType2 &key2)
   {  return base_t::get()(this->key_forward(key1), this->key_forward(key2));  }

   const ValueTraits *const traits_;
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP
