/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2013.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
// The internal implementation of red-black trees is based on that of SGI STL
// stl_tree.h file:
//
// Copyright (c) 1996,1997
// Silicon Graphics Computer Systems, Inc.
//
// Permission to use, copy, modify, distribute and sell this software
// and its documentation for any purpose is hereby granted without fee,
// provided that the above copyright notice appear in all copies and
// that both that copyright notice and this permission notice appear
// in supporting documentation.  Silicon Graphics makes no
// representations about the suitability of this software for any
// purpose.  It is provided "as is" without express or implied warranty.
//
//
// Copyright (c) 1994
// Hewlett-Packard Company
//
// Permission to use, copy, modify, distribute and sell this software
// and its documentation for any purpose is hereby granted without fee,
// provided that the above copyright notice appear in all copies and
// that both that copyright notice and this permission notice appear
// in supporting documentation.  Hewlett-Packard Company makes no
// representations about the suitability of this software for any
// purpose.  It is provided "as is" without express or implied warranty.
//
// The tree destruction algorithm is based on Julienne Walker and The EC Team code:
//
// This code is in the public domain. Anyone may use it or change it in any way that
// they see fit. The author assumes no responsibility for damages incurred through
// use of the original code or any variations thereof.
//
// It is requested, but not required, that due credit is given to the original author
// and anyone who has modified the code through a header comment, such as this one.

#ifndef BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>

#include <cstddef>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost {
namespace intrusive {

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

template<class NodeTraits, class F>
struct rbtree_node_cloner
   :  private detail::ebo_functor_holder<F>
{
   typedef typename NodeTraits::node_ptr  node_ptr;
   typedef detail::ebo_functor_holder<F>  base_t;

   rbtree_node_cloner(F f)
      :  base_t(f)
   {}

   node_ptr operator()(const node_ptr & p)
   {
      node_ptr n = base_t::get()(p);
      NodeTraits::set_color(n, NodeTraits::get_color(p));
      return n;
   }
};

template<class NodeTraits>
struct rbtree_erase_fixup
{
   typedef typename NodeTraits::node_ptr  node_ptr;
   typedef typename NodeTraits::color     color;

   void operator()(const node_ptr & to_erase, const node_ptr & successor)
   {
      //Swap color of y and z
      color tmp(NodeTraits::get_color(successor));
      NodeTraits::set_color(successor, NodeTraits::get_color(to_erase));
      NodeTraits::set_color(to_erase, tmp);
   }
};

#endif   //#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

//! rbtree_algorithms provides basic algorithms to manipulate
//! nodes forming a red-black tree. The insertion and deletion algorithms are
//! based on those in Cormen, Leiserson, and Rivest, Introduction to Algorithms
//! (MIT Press, 1990), except that
//!
//! (1) the header node is maintained with links not only to the root
//! but also to the leftmost node of the tree, to enable constant time
//! begin(), and to the rightmost node of the tree, to enable linear time
//! performance when used with the generic set algorithms (set_union,
//! etc.);
//!
//! (2) when a node being deleted has two children its successor node is
//! relinked into its place, rather than copied, so that the only
//! pointers invalidated are those referring to the deleted node.
//!
//! rbtree_algorithms is configured with a NodeTraits class, which encapsulates the
//! information about the node to be manipulated. NodeTraits must support the
//! following interface:
//!
//! <b>Typedefs</b>:
//!
//! <tt>node</tt>: The type of the node that forms the binary search tree
//!
//! <tt>node_ptr</tt>: A pointer to a node
//!
//! <tt>const_node_ptr</tt>: A pointer to a const node
//!
//! <tt>color</tt>: The type that can store the color of a node
//!
//! <b>Static functions</b>:
//!
//! <tt>static node_ptr get_parent(const_node_ptr n);</tt>
//!
//! <tt>static void set_parent(node_ptr n, node_ptr parent);</tt>
//!
//! <tt>static node_ptr get_left(const_node_ptr n);</tt>
//!
//! <tt>static void set_left(node_ptr n, node_ptr left);</tt>
//!
//! <tt>static node_ptr get_right(const_node_ptr n);</tt>
//!
//! <tt>static void set_right(node_ptr n, node_ptr right);</tt>
//!
//! <tt>static color get_color(const_node_ptr n);</tt>
//!
//! <tt>static void set_color(node_ptr n, color c);</tt>
//!
//! <tt>static color black();</tt>
//!
//! <tt>static color red();</tt>
template<class NodeTraits>
class rbtree_algorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public bstree_algorithms<NodeTraits>
   #endif
{
   public:
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node            node;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;
   typedef typename NodeTraits::color           color;

   /// @cond
   private:

   typedef bstree_algorithms<NodeTraits>  bstree_algo;

   /// @endcond

   public:

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename bstree_algo::insert_commit_data insert_commit_data;

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::get_header(const const_node_ptr&)
   static node_ptr get_header(const const_node_ptr & n);

   //! @copydoc ::boost::intrusive::bstree_algorithms::begin_node
   static node_ptr begin_node(const const_node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::end_node
   static node_ptr end_node(const const_node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_tree
   static void swap_tree(const node_ptr & header1, const node_ptr & header2);
   
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(const node_ptr&,const node_ptr&)
   static void swap_nodes(const node_ptr & node1, const node_ptr & node2)
   {
      if(node1 == node2)
         return;

      node_ptr header1(bstree_algo::get_header(node1)), header2(bstree_algo::get_header(node2));
      swap_nodes(node1, header1, node2, header2);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(const node_ptr&,const node_ptr&,const node_ptr&,const node_ptr&)
   static void swap_nodes(const node_ptr & node1, const node_ptr & header1, const node_ptr & node2, const node_ptr & header2)
   {
      if(node1 == node2)   return;

      bstree_algo::swap_nodes(node1, header1, node2, header2);
      //Swap color
      color c = NodeTraits::get_color(node1);
      NodeTraits::set_color(node1, NodeTraits::get_color(node2));
      NodeTraits::set_color(node2, c);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(const node_ptr&,const node_ptr&)
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & new_node)
   {
      if(node_to_be_replaced == new_node)
         return;
      replace_node(node_to_be_replaced, bstree_algo::get_header(node_to_be_replaced), new_node);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(const node_ptr&,const node_ptr&,const node_ptr&)
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & header, const node_ptr & new_node)
   {
      bstree_algo::replace_node(node_to_be_replaced, header, new_node);
      NodeTraits::set_color(new_node, NodeTraits::get_color(node_to_be_replaced));
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(const node_ptr&)
   static void unlink(const node_ptr& node)
   {
      node_ptr x = NodeTraits::get_parent(node);
      if(x){
         while(!is_header(x))
            x = NodeTraits::get_parent(x);
         erase(x, node);
      }
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink_leftmost_without_rebalance
   static node_ptr unlink_leftmost_without_rebalance(const node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::unique(const const_node_ptr&)
   static bool unique(const const_node_ptr & node);

   //! @copydoc ::boost::intrusive::bstree_algorithms::size(const const_node_ptr&)
   static std::size_t size(const const_node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::next_node(const node_ptr&)
   static node_ptr next_node(const node_ptr & node);

   //! @copydoc ::boost::intrusive::bstree_algorithms::prev_node(const node_ptr&)
   static node_ptr prev_node(const node_ptr & node);

   //! @copydoc ::boost::intrusive::bstree_algorithms::init(const node_ptr&)
   static void init(const node_ptr & node);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::init_header(const node_ptr&)
   static void init_header(const node_ptr & header)
   {
      bstree_algo::init_header(header);
      NodeTraits::set_color(header, NodeTraits::red());
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(const node_ptr&,const node_ptr&)
   static node_ptr erase(const node_ptr & header, const node_ptr & z)
   {
      typename bstree_algo::data_for_rebalance info;
      bstree_algo::erase(header, z, rbtree_erase_fixup<NodeTraits>(), info);

      //Rebalance rbtree
      if(NodeTraits::get_color(z) != NodeTraits::red()){
         rebalance_after_erasure(header, info.x, info.x_parent);
      }
      return z;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const const_node_ptr&,const node_ptr&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (const const_node_ptr & source_header, const node_ptr & target_header, Cloner cloner, Disposer disposer)
   {
      rbtree_node_cloner<NodeTraits, Cloner> new_cloner(cloner);
      bstree_algo::clone(source_header, target_header, new_cloner, disposer);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clear_and_dispose(const node_ptr&,Disposer)
   template<class Disposer>
   static void clear_and_dispose(const node_ptr & header, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr lower_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr upper_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const const_node_ptr&, const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr find
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> equal_range
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const const_node_ptr&,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> bounded_range
      (const const_node_ptr & header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count(const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_upper_bound(const node_ptr&,const node_ptr&,NodePtrCompare)
   template<class NodePtrCompare>
   static node_ptr insert_equal_upper_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp)
   {
      bstree_algo::insert_equal_upper_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_lower_bound(const node_ptr&,const node_ptr&,NodePtrCompare)
   template<class NodePtrCompare>
   static node_ptr insert_equal_lower_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp)
   {
      bstree_algo::insert_equal_lower_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal(const node_ptr&,const node_ptr&,const node_ptr&,NodePtrCompare)
   template<class NodePtrCompare>
   static node_ptr insert_equal
      (const node_ptr & header, const node_ptr & hint, const node_ptr & new_node, NodePtrCompare comp)
   {
      bstree_algo::insert_equal(header, hint, new_node, comp);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_before(const node_ptr&,const node_ptr&,const node_ptr&)
   static node_ptr insert_before
      (const node_ptr & header, const node_ptr & pos, const node_ptr & new_node)
   {
      bstree_algo::insert_before(header, pos, new_node);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_back(const node_ptr&,const node_ptr&)
   static void push_back(const node_ptr & header, const node_ptr & new_node)
   {
      bstree_algo::push_back(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_front(const node_ptr&,const node_ptr&)
   static void push_front(const node_ptr & header, const node_ptr & new_node)
   {
      bstree_algo::push_front(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const const_node_ptr&,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const const_node_ptr & header,  const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const const_node_ptr&,const node_ptr&,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const const_node_ptr & header, const node_ptr &hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(const node_ptr&,const node_ptr&,const insert_commit_data&)
   static void insert_unique_commit
      (const node_ptr & header, const node_ptr & new_value, const insert_commit_data &commit_data)
   {
      bstree_algo::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(header, new_value);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const const_node_ptr & p)
   {
      return NodeTraits::get_color(p) == NodeTraits::red() &&
            bstree_algo::is_header(p);
   }

   /// @cond
   private:

   static void rebalance_after_erasure(const node_ptr & header, node_ptr x, node_ptr x_parent)
   {
      while(x != NodeTraits::get_parent(header) && (!x || NodeTraits::get_color(x) == NodeTraits::black())){
         if(x == NodeTraits::get_left(x_parent)){
            node_ptr w = NodeTraits::get_right(x_parent);
            BOOST_ASSERT(w);
            if(NodeTraits::get_color(w) == NodeTraits::red()){
               NodeTraits::set_color(w, NodeTraits::black());
               NodeTraits::set_color(x_parent, NodeTraits::red());
               bstree_algo::rotate_left(x_parent, header);
               w = NodeTraits::get_right(x_parent);
            }
            if((!NodeTraits::get_left(w) || NodeTraits::get_color(NodeTraits::get_left(w))  == NodeTraits::black()) &&
               (!NodeTraits::get_right(w) || NodeTraits::get_color(NodeTraits::get_right(w)) == NodeTraits::black())){
               NodeTraits::set_color(w, NodeTraits::red());
               x = x_parent;
               x_parent = NodeTraits::get_parent(x_parent);
            }
            else {
               if(!NodeTraits::get_right(w) || NodeTraits::get_color(NodeTraits::get_right(w)) == NodeTraits::black()){
                  NodeTraits::set_color(NodeTraits::get_left(w), NodeTraits::black());
                  NodeTraits::set_color(w, NodeTraits::red());
                  bstree_algo::rotate_right(w, header);
                  w = NodeTraits::get_right(x_parent);
               }
               NodeTraits::set_color(w, NodeTraits::get_color(x_parent));
               NodeTraits::set_color(x_parent, NodeTraits::black());
               if(NodeTraits::get_right(w))
                  NodeTraits::set_color(NodeTraits::get_right(w), NodeTraits::black());
               bstree_algo::rotate_left(x_parent, header);
               break;
            }
         }
         else {
            // same as above, with right_ <-> left_.
            node_ptr w = NodeTraits::get_left(x_parent);
            if(NodeTraits::get_color(w) == NodeTraits::red()){
               NodeTraits::set_color(w, NodeTraits::black());
               NodeTraits::set_color(x_parent, NodeTraits::red());
               bstree_algo::rotate_right(x_parent, header);
               w = NodeTraits::get_left(x_parent);
            }
            if((!NodeTraits::get_right(w) || NodeTraits::get_color(NodeTraits::get_right(w)) == NodeTraits::black()) &&
               (!NodeTraits::get_left(w) || NodeTraits::get_color(NodeTraits::get_left(w)) == NodeTraits::black())){
               NodeTraits::set_color(w, NodeTraits::red());
               x = x_parent;
               x_parent = NodeTraits::get_parent(x_parent);
            }
            else {
               if(!NodeTraits::get_left(w) || NodeTraits::get_color(NodeTraits::get_left(w)) == NodeTraits::black()){
                  NodeTraits::set_color(NodeTraits::get_right(w), NodeTraits::black());
                  NodeTraits::set_color(w, NodeTraits::red());
                  bstree_algo::rotate_left(w, header);
                  w = NodeTraits::get_left(x_parent);
               }
               NodeTraits::set_color(w, NodeTraits::get_color(x_parent));
               NodeTraits::set_color(x_parent, NodeTraits::black());
               if(NodeTraits::get_left(w))
                  NodeTraits::set_color(NodeTraits::get_left(w), NodeTraits::black());
               bstree_algo::rotate_right(x_parent, header);
               break;
            }
         }
      }
      if(x)
         NodeTraits::set_color(x, NodeTraits::black());
   }

   static void rebalance_after_insertion(const node_ptr & header, node_ptr p)
   {
      NodeTraits::set_color(p, NodeTraits::red());
      while(p != NodeTraits::get_parent(header) && NodeTraits::get_color(NodeTraits::get_parent(p)) == NodeTraits::red()){
         node_ptr p_parent(NodeTraits::get_parent(p));
         node_ptr p_parent_parent(NodeTraits::get_parent(p_parent));
         if(bstree_algo::is_left_child(p_parent)){
            node_ptr x = NodeTraits::get_right(p_parent_parent);
            if(x && NodeTraits::get_color(x) == NodeTraits::red()){
               NodeTraits::set_color(p_parent, NodeTraits::black());
               NodeTraits::set_color(p_parent_parent, NodeTraits::red());
               NodeTraits::set_color(x, NodeTraits::black());
               p = p_parent_parent;
            }
            else {
               if(!bstree_algo::is_left_child(p)){
                  p = p_parent;
                  bstree_algo::rotate_left(p, header);
               }
               node_ptr new_p_parent(NodeTraits::get_parent(p));
               node_ptr new_p_parent_parent(NodeTraits::get_parent(new_p_parent));
               NodeTraits::set_color(new_p_parent, NodeTraits::black());
               NodeTraits::set_color(new_p_parent_parent, NodeTraits::red());
               bstree_algo::rotate_right(new_p_parent_parent, header);
            }
         }
         else{
            node_ptr x = NodeTraits::get_left(p_parent_parent);
            if(x && NodeTraits::get_color(x) == NodeTraits::red()){
               NodeTraits::set_color(p_parent, NodeTraits::black());
               NodeTraits::set_color(p_parent_parent, NodeTraits::red());
               NodeTraits::set_color(x, NodeTraits::black());
               p = p_parent_parent;
            }
            else{
               if(bstree_algo::is_left_child(p)){
                  p = p_parent;
                  bstree_algo::rotate_right(p, header);
               }
               node_ptr new_p_parent(NodeTraits::get_parent(p));
               node_ptr new_p_parent_parent(NodeTraits::get_parent(new_p_parent));
               NodeTraits::set_color(new_p_parent, NodeTraits::black());
               NodeTraits::set_color(new_p_parent_parent, NodeTraits::red());
               bstree_algo::rotate_left(new_p_parent_parent, header);
            }
         }
      }
      NodeTraits::set_color(NodeTraits::get_parent(header), NodeTraits::black());
   }
   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<RbTreeAlgorithms, NodeTraits>
{
   typedef rbtree_algorithms<NodeTraits> type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP
