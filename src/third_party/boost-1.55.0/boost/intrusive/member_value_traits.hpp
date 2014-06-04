/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
#define BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP

#include <boost/intrusive/link_mode.hpp>
#include <iterator>
#include <boost/intrusive/detail/parent_from_member.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost {
namespace intrusive {

//!This value traits template is used to create value traits
//!from user defined node traits where value_traits::value_type will
//!store a node_traits::node
template< class T, class NodeTraits
        , typename NodeTraits::node T::* PtrToMember
        , link_mode_type LinkMode = safe_link>
struct member_value_traits
{
   public:
   typedef NodeTraits                                                   node_traits;
   typedef T                                                            value_type;
   typedef typename node_traits::node                                   node;
   typedef typename node_traits::node_ptr                               node_ptr;
   typedef typename node_traits::const_node_ptr                         const_node_ptr;
   typedef typename pointer_traits<node_ptr>::template
      rebind_pointer<T>::type                                           pointer;
   typedef typename pointer_traits<node_ptr>::template
      rebind_pointer<const T>::type                                     const_pointer;
   //typedef typename pointer_traits<pointer>::reference                  reference;
   //typedef typename pointer_traits<const_pointer>::reference            const_reference;
   typedef value_type &                                                 reference;
   typedef const value_type &                                           const_reference;
   static const link_mode_type link_mode = LinkMode;

   static node_ptr to_node_ptr(reference value)
   {  return node_ptr(&(value.*PtrToMember));   }

   static const_node_ptr to_node_ptr(const_reference value)
   {  return node_ptr(&(value.*PtrToMember));   }

   static pointer to_value_ptr(const node_ptr &n)
   {
      return pointer(detail::parent_from_member<value_type, node>
         (boost::intrusive::detail::to_raw_pointer(n), PtrToMember));
   }

   static const_pointer to_value_ptr(const const_node_ptr &n)
   {
      return pointer(detail::parent_from_member<value_type, node>
         (boost::intrusive::detail::to_raw_pointer(n), PtrToMember));
   }
};

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
