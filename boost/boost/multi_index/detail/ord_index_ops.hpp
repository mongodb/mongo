/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 *
 * The internal implementation of red-black trees is based on that of SGI STL
 * stl_tree.h file: 
 *
 * Copyright (c) 1996,1997
 * Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Silicon Graphics makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 *
 * Copyright (c) 1994
 * Hewlett-Packard Company
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Hewlett-Packard Company makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_ORD_INDEX_OPS_HPP
#define BOOST_MULTI_INDEX_DETAIL_ORD_INDEX_OPS_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* Common code for index memfuns having templatized and
 * non-templatized versions.
 */

template<
  typename Node,typename KeyFromValue,
  typename CompatibleKey,typename CompatibleCompare
>
inline Node* ordered_index_find(
  Node* header,const KeyFromValue& key,const CompatibleKey& x,
  const CompatibleCompare& comp)
{
  Node* y=header;
  Node* z=Node::from_impl(header->parent());
    
  while (z){
    if(!comp(key(z->value()),x)){
      y=z;
      z=Node::from_impl(z->left());
    }
    else z=Node::from_impl(z->right());
  }
    
  return (y==header||comp(x,key(y->value())))?header:y;
}

template<
  typename Node,typename KeyFromValue,
  typename CompatibleKey,typename CompatibleCompare
>
inline Node* ordered_index_lower_bound(
  Node* header,const KeyFromValue& key,const CompatibleKey& x,
  const CompatibleCompare& comp)
{
  Node* y=header;
  Node* z=Node::from_impl(header->parent());

  while(z){
    if(!comp(key(z->value()),x)){
      y=z;
      z=Node::from_impl(z->left());
    }
    else z=Node::from_impl(z->right());
  }

  return y;
}

template<
  typename Node,typename KeyFromValue,
  typename CompatibleKey,typename CompatibleCompare
>
inline Node* ordered_index_upper_bound(
  Node* header,const KeyFromValue& key,const CompatibleKey& x,
  const CompatibleCompare& comp)
{
  Node* y=header;
  Node* z=Node::from_impl(header->parent());

  while(z){
    if(comp(x,key(z->value()))){
      y=z;
      z=Node::from_impl(z->left());
    }
    else z=Node::from_impl(z->right());
  }

  return y;
}

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif
