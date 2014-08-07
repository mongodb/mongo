/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
// The implementation of splay trees is based on the article and code published
// in C++ Users Journal "Implementing Splay Trees in C++" (September 1, 2005).
//
// The splay code has been modified and (supposedly) improved by Ion Gaztanaga.
//
// Here is the copyright notice of the original file containing the splay code:
//
//  splay_tree.h -- implementation of a STL compatible splay tree.
//
//  Copyright (c) 2004 Ralf Mattethat
//
//  Permission to copy, use, modify, sell and distribute this software
//  is granted provided this copyright notice appears in all copies.
//  This software is provided "as is" without express or implied
//  warranty, and with no claim as to its suitability for any purpose.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <cstddef>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

namespace boost {
namespace intrusive {

/// @cond
namespace detail {

template<class NodeTraits>
struct splaydown_rollback
{
   typedef typename NodeTraits::node_ptr node_ptr;
   splaydown_rollback( const node_ptr *pcur_subtree, const node_ptr & header
                     , const node_ptr & leftmost           , const node_ptr & rightmost)
      : pcur_subtree_(pcur_subtree)  , header_(header)
      , leftmost_(leftmost)   , rightmost_(rightmost)
   {}

   void release()
   {  pcur_subtree_ = 0;  }

   ~splaydown_rollback()
   {
      if(pcur_subtree_){
         //Exception can only be thrown by comp, but
         //tree invariants still hold. *pcur_subtree is the current root
         //so link it to the header.
         NodeTraits::set_parent(*pcur_subtree_, header_);
         NodeTraits::set_parent(header_, *pcur_subtree_);
         //Recover leftmost/rightmost pointers
         NodeTraits::set_left (header_, leftmost_);
         NodeTraits::set_right(header_, rightmost_);
      }
   }
   const node_ptr *pcur_subtree_;
   node_ptr header_, leftmost_, rightmost_;
};

}  //namespace detail {
/// @endcond

//!   A splay tree is an implementation of a binary search tree. The tree is
//!   self balancing using the splay algorithm as described in
//!
//!      "Self-Adjusting Binary Search Trees
//!      by Daniel Dominic Sleator and Robert Endre Tarjan
//!      AT&T Bell Laboratories, Murray Hill, NJ
//!      Journal of the ACM, Vol 32, no 3, July 1985, pp 652-686
//!
//! splaytree_algorithms is configured with a NodeTraits class, which encapsulates the
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
template<class NodeTraits>
class splaytree_algorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public bstree_algorithms<NodeTraits>
   #endif
{
   /// @cond
   private:
   typedef bstree_algorithms<NodeTraits> bstree_algo;
   /// @endcond

   public:
   typedef typename NodeTraits::node            node;
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename bstree_algo::insert_commit_data insert_commit_data;

   public:
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::get_header(const const_node_ptr&)
   static node_ptr get_header(const const_node_ptr & n);

   //! @copydoc ::boost::intrusive::bstree_algorithms::begin_node
   static node_ptr begin_node(const const_node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::end_node
   static node_ptr end_node(const const_node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_tree
   static void swap_tree(const node_ptr & header1, const node_ptr & header2);

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(const node_ptr&,const node_ptr&)
   static void swap_nodes(const node_ptr & node1, const node_ptr & node2);

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(const node_ptr&,const node_ptr&,const node_ptr&,const node_ptr&)
   static void swap_nodes(const node_ptr & node1, const node_ptr & header1, const node_ptr & node2, const node_ptr & header2);

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(const node_ptr&,const node_ptr&)
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & new_node);

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(const node_ptr&,const node_ptr&,const node_ptr&)
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & header, const node_ptr & new_node);

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(const node_ptr&)
   static void unlink(const node_ptr & node);

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

   //! @copydoc ::boost::intrusive::bstree_algorithms::init_header(const node_ptr&)
   static void init_header(const node_ptr & header);
   
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(const node_ptr&,const node_ptr&)
   //! Additional notes: the previous node of z is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   static void erase(const node_ptr & header, const node_ptr & z, bool splay = true)
   {
      //posibility 1
      if(splay && NodeTraits::get_left(z)){
         splay_up(bstree_algo::prev_node(z), header);
      }
      /*
      //possibility 2
      if(splay && NodeTraits::get_left(z)){
         node_ptr l = NodeTraits::get_left(z);
         splay_up(l, header);
      }*//*
      if(splay && NodeTraits::get_left(z)){
         node_ptr l = bstree_algo::prev_node(z);
         splay_up_impl(l, z);
      }*/
      /*
      //possibility 4
      if(splay){
         splay_up(z, header);
      }*/

      //if(splay)
         //splay_up(z, header);
      bstree_algo::erase(header, z);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const const_node_ptr&,const node_ptr&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (const const_node_ptr & source_header, const node_ptr & target_header, Cloner cloner, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree_algorithms::clear_and_dispose(const node_ptr&,Disposer)
   template<class Disposer>
   static void clear_and_dispose(const node_ptr & header, Disposer disposer);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count
      (const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {
      std::pair<node_ptr, node_ptr> ret = equal_range(header, key, comp);
      std::size_t n = 0;
      while(ret.first != ret.second){
         ++n;
         ret.first = next_node(ret.first);
      }
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return bstree_algo::count(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr lower_bound
      (const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //splay_down(detail::uncast(header), key, comp);
      node_ptr y = bstree_algo::lower_bound(header, key, comp);
      if(splay) splay_up(y, detail::uncast(header));
      return y;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr lower_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return bstree_algo::lower_bound(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr upper_bound
      (const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //splay_down(detail::uncast(header), key, comp);
      node_ptr y = bstree_algo::upper_bound(header, key, comp);
      if(splay) splay_up(y, detail::uncast(header));
      return y;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr upper_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return bstree_algo::upper_bound(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const const_node_ptr&, const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the found node of the lower bound is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr find
      (const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      if(splay) splay_down(detail::uncast(header), key, comp);
      node_ptr end = detail::uncast(header);
      node_ptr y = bstree_algo::lower_bound(header, key, comp);
      node_ptr r = (y == end || comp(key, y)) ? end : y;
      return r;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const const_node_ptr&, const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr find
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return bstree_algo::find(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> equal_range
      (const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //splay_down(detail::uncast(header), key, comp);
      std::pair<node_ptr, node_ptr> ret = bstree_algo::equal_range(header, key, comp);
      if(splay) splay_up(ret.first, detail::uncast(header));
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> equal_range
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return bstree_algo::equal_range(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const const_node_ptr&,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   //! Additional notes: the first node of the range is splayed. The "splay" parameter which indicated if splaying
   //! should be performed, it's deprecated and will disappear in future versions.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> bounded_range
      (const node_ptr & header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed, bool splay = true)
   {
      std::pair<node_ptr, node_ptr> ret =
         bstree_algo::bounded_range(header, lower_key, upper_key, comp, left_closed, right_closed);
      if(splay) splay_up(ret.first, detail::uncast(header));
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const const_node_ptr&,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> bounded_range
      (const const_node_ptr & header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed)
   {  return bstree_algo::bounded_range(header, lower_key, upper_key, comp, left_closed, right_closed);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_upper_bound(const node_ptr&,const node_ptr&,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static node_ptr insert_equal_upper_bound
      (const node_ptr & header, const node_ptr & new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return bstree_algo::insert_equal_upper_bound(header, new_node, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_lower_bound(const node_ptr&,const node_ptr&,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static node_ptr insert_equal_lower_bound
      (const node_ptr & header, const node_ptr & new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return bstree_algo::insert_equal_lower_bound(header, new_node, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal(const node_ptr&,const node_ptr&,const node_ptr&,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static node_ptr insert_equal
      (const node_ptr & header, const node_ptr & hint, const node_ptr & new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return bstree_algo::insert_equal(header, hint, new_node, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_before(const node_ptr&,const node_ptr&,const node_ptr&)
   //! Additional note: the inserted node is splayed
   static node_ptr insert_before
      (const node_ptr & header, const node_ptr & pos, const node_ptr & new_node)
   {
      bstree_algo::insert_before(header, pos, new_node);
      splay_up(new_node, header);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_back(const node_ptr&,const node_ptr&)
   //! Additional note: the inserted node is splayed
   static void push_back(const node_ptr & header, const node_ptr & new_node)
   {
      bstree_algo::push_back(header, new_node);
      splay_up(new_node, header);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_front(const node_ptr&,const node_ptr&)
   //! Additional note: the inserted node is splayed
   static void push_front(const node_ptr & header, const node_ptr & new_node)
   {
      bstree_algo::push_front(header, new_node);
      splay_up(new_node, header);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const const_node_ptr&,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   //! Additional note: nodes with the given key are splayed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const node_ptr & header, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {
      splay_down(header, key, comp);
      return bstree_algo::insert_unique_check(header, key, comp, commit_data);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const const_node_ptr&,const node_ptr&,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   //! Additional note: nodes with the given key are splayed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const node_ptr & header, const node_ptr &hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {
      splay_down(header, key, comp);
      return bstree_algo::insert_unique_check(header, hint, key, comp, commit_data);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(const node_ptr&,const node_ptr&,const insert_commit_data&)
   static void insert_unique_commit
      (const node_ptr & header, const node_ptr & new_value, const insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const const_node_ptr & p);

   //! @copydoc ::boost::intrusive::bstree_algorithms::rebalance
   static void rebalance(const node_ptr & header);

   //! @copydoc ::boost::intrusive::bstree_algorithms::rebalance_subtree
   static node_ptr rebalance_subtree(const node_ptr & old_root);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   // bottom-up splay, use data_ as parent for n    | complexity : logarithmic    | exception : nothrow
   static void splay_up(const node_ptr & node, const node_ptr & header)
   {
      // If (node == header) do a splay for the right most node instead
      // this is to boost performance of equal_range/count on equivalent containers in the case
      // where there are many equal elements at the end
      node_ptr n((node == header) ? NodeTraits::get_right(header) : node);
      node_ptr t(header);

      if( n == t ) return;

      for( ;; ){
         node_ptr p(NodeTraits::get_parent(n));
         node_ptr g(NodeTraits::get_parent(p));

         if( p == t )   break;

         if( g == t ){
            // zig
            rotate(n);
         }
         else if ((NodeTraits::get_left(p) == n && NodeTraits::get_left(g) == p)    ||
                  (NodeTraits::get_right(p) == n && NodeTraits::get_right(g) == p)  ){
            // zig-zig
            rotate(p);
            rotate(n);
         }
         else{
            // zig-zag
            rotate(n);
            rotate(n);
         }
      }
   }

   // top-down splay | complexity : logarithmic    | exception : strong, note A
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr splay_down(const node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {
      if(!NodeTraits::get_parent(header))
         return header;
      //Most splay tree implementations use a dummy/null node to implement.
      //this function. This has some problems for a generic library like Intrusive:
      //
      // * The node might not have a default constructor.
      // * The default constructor could throw.
      //
      //We already have a header node. Leftmost and rightmost nodes of the tree
      //are not changed when splaying (because the invariants of the tree don't
      //change) We can back up them, use the header as the null node and
      //reassign old values after the function has been completed.
      node_ptr t = NodeTraits::get_parent(header);
      //Check if tree has a single node
      if(!NodeTraits::get_left(t) && !NodeTraits::get_right(t))
         return t;
      //Backup leftmost/rightmost
      node_ptr leftmost (NodeTraits::get_left(header));
      node_ptr rightmost(NodeTraits::get_right(header));
      {
         //Anti-exception rollback, recovers the original header node if an exception is thrown.
         detail::splaydown_rollback<NodeTraits> rollback(&t, header, leftmost, rightmost);
         node_ptr null_node = header;
         node_ptr l = null_node;
         node_ptr r = null_node;

         for( ;; ){
            if(comp(key, t)){
               if(NodeTraits::get_left(t) == node_ptr() )
                  break;
               if(comp(key, NodeTraits::get_left(t))){
                  t = bstree_algo::rotate_right(t);

                  if(NodeTraits::get_left(t) == node_ptr())
                     break;
                  link_right(t, r);
               }
               else if(comp(NodeTraits::get_left(t), key)){
                  link_right(t, r);

                  if(NodeTraits::get_right(t) == node_ptr() )
                     break;
                  link_left(t, l);
               }
               else{
                  link_right(t, r);
               }
            }
            else if(comp(t, key)){
               if(NodeTraits::get_right(t) == node_ptr() )
                  break;

               if(comp(NodeTraits::get_right(t), key)){
                     t = bstree_algo::rotate_left( t );

                     if(NodeTraits::get_right(t) == node_ptr() )
                        break;
                     link_left(t, l);
               }
               else if(comp(key, NodeTraits::get_right(t))){
                  link_left(t, l);

                  if(NodeTraits::get_left(t) == node_ptr())
                     break;

                  link_right(t, r);
               }
               else{
                  link_left(t, l);
               }
            }
            else{
               break;
            }
         }

         assemble(t, l, r, null_node);
         rollback.release();
      }

      //Now recover the original header except for the
      //splayed root node.
      //t is the current root
      NodeTraits::set_parent(header, t);
      NodeTraits::set_parent(t, header);
      //Recover leftmost/rightmost pointers
      NodeTraits::set_left (header, leftmost);
      NodeTraits::set_right(header, rightmost);
      return t;
   }

   private:

   /// @cond

   // assemble the three sub-trees into new tree pointed to by t    | complexity : constant        | exception : nothrow
   static void assemble(const node_ptr &t, const node_ptr & l, const node_ptr & r, const const_node_ptr & null_node )
   {
      NodeTraits::set_right(l, NodeTraits::get_left(t));
      NodeTraits::set_left(r, NodeTraits::get_right(t));

      if(NodeTraits::get_right(l) != node_ptr()){
         NodeTraits::set_parent(NodeTraits::get_right(l), l);
      }

      if(NodeTraits::get_left(r) != node_ptr()){
         NodeTraits::set_parent(NodeTraits::get_left(r), r);
      }

      NodeTraits::set_left (t, NodeTraits::get_right(null_node));
      NodeTraits::set_right(t, NodeTraits::get_left(null_node));

      if( NodeTraits::get_left(t) != node_ptr() ){
         NodeTraits::set_parent(NodeTraits::get_left(t), t);
      }

      if( NodeTraits::get_right(t) ){
         NodeTraits::set_parent(NodeTraits::get_right(t), t);
      }
   }

   // break link to left child node and attach it to left tree pointed to by l   | complexity : constant | exception : nothrow
   static void link_left(node_ptr & t, node_ptr & l)
   {
      NodeTraits::set_right(l, t);
      NodeTraits::set_parent(t, l);
      l = t;
      t = NodeTraits::get_right(t);
   }

   // break link to right child node and attach it to right tree pointed to by r | complexity : constant | exception : nothrow
   static void link_right(node_ptr & t, node_ptr & r)
   {
      NodeTraits::set_left(r, t);
      NodeTraits::set_parent(t, r);
      r = t;
      t = NodeTraits::get_left(t);
   }

   // rotate n with its parent                     | complexity : constant    | exception : nothrow
   static void rotate(const node_ptr & n)
   {
      node_ptr p = NodeTraits::get_parent(n);
      node_ptr g = NodeTraits::get_parent(p);
      //Test if g is header before breaking tree
      //invariants that would make is_header invalid
      bool g_is_header = bstree_algo::is_header(g);

      if(NodeTraits::get_left(p) == n){
         NodeTraits::set_left(p, NodeTraits::get_right(n));
         if(NodeTraits::get_left(p) != node_ptr())
            NodeTraits::set_parent(NodeTraits::get_left(p), p);
         NodeTraits::set_right(n, p);
      }
      else{ // must be ( p->right == n )
         NodeTraits::set_right(p, NodeTraits::get_left(n));
         if(NodeTraits::get_right(p) != node_ptr())
            NodeTraits::set_parent(NodeTraits::get_right(p), p);
         NodeTraits::set_left(n, p);
      }

      NodeTraits::set_parent(p, n);
      NodeTraits::set_parent(n, g);

      if(g_is_header){
         if(NodeTraits::get_parent(g) == p)
            NodeTraits::set_parent(g, n);
         else{//must be ( g->right == p )
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);
            NodeTraits::set_right(g, n);
         }
      }
      else{
         if(NodeTraits::get_left(g) == p)
            NodeTraits::set_left(g, n);
         else  //must be ( g->right == p )
            NodeTraits::set_right(g, n);
      }
   }

   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<SplayTreeAlgorithms, NodeTraits>
{
   typedef splaytree_algorithms<NodeTraits> type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
