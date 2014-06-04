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

#ifndef BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
#define BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP

#include <boost/intrusive/link_mode.hpp>
#include <boost/pointer_cast.hpp>
#include <boost/pointer_to_other.hpp>
#include <iterator>

namespace boost {
namespace intrusive {

//!This value traits template is used to create value traits
//!from user defined node traits where value_traits::value_type will
//!derive from node_traits::node
template<class T, class NodeTraits, link_mode_type LinkMode = safe_link>
struct derivation_value_traits
{
   public:
   typedef NodeTraits                                                node_traits;
   typedef T                                                         value_type;
   typedef typename node_traits::node                                node;
   typedef typename node_traits::node_ptr                            node_ptr;
   typedef typename node_traits::const_node_ptr                      const_node_ptr;
   typedef typename boost::pointer_to_other<node_ptr, T>::type       pointer;
   typedef typename boost::pointer_to_other<node_ptr, const T>::type const_pointer;
   typedef typename boost::intrusive::
      pointer_traits<pointer>::reference                             reference;
   typedef typename boost::intrusive::
      pointer_traits<const_pointer>::reference                       const_reference;
   static const link_mode_type link_mode = LinkMode;

   static node_ptr to_node_ptr(reference value)
   { return node_ptr(&value); }

   static const_node_ptr to_node_ptr(const_reference value)
   { return node_ptr(&value); }

   static pointer to_value_ptr(const node_ptr &n)
   {
//      This still fails in gcc < 4.4 so forget about it
//      using ::boost::static_pointer_cast;
//      return static_pointer_cast<value_type>(n));
      return pointer(&static_cast<value_type&>(*n));
   }

   static const_pointer to_value_ptr(const const_node_ptr &n)
   {
//      This still fails in gcc < 4.4 so forget about it
//      using ::boost::static_pointer_cast;
//      return static_pointer_cast<const value_type>(n));
      return const_pointer(&static_cast<const value_type&>(*n));
   }
};

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
