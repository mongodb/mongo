/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Daniel K. O. 2005.
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>

#include <cstddef>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>
#include <boost/intrusive/pointer_traits.hpp>


namespace boost {
namespace intrusive {

/// @cond

template<class NodeTraits, class F>
struct avltree_node_cloner
   :  private detail::ebo_functor_holder<F>
{
   typedef typename NodeTraits::node_ptr  node_ptr;
   typedef detail::ebo_functor_holder<F>  base_t;

   avltree_node_cloner(F f)
      :  base_t(f)
   {}

   node_ptr operator()(const node_ptr & p)
   {
      node_ptr n = base_t::get()(p);
      NodeTraits::set_balance(n, NodeTraits::get_balance(p));
      return n;
   }
};

template<class NodeTraits>
struct avltree_erase_fixup
{
   typedef typename NodeTraits::node_ptr  node_ptr;

   void operator()(const node_ptr & to_erase, const node_ptr & successor)
   {  NodeTraits::set_balance(successor, NodeTraits::get_balance(to_erase));  }
};

/// @endcond

//! avltree_algorithms is configured with a NodeTraits class, which encapsulates the
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
//! <tt>balance</tt>: The type of the balance factor
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
//! <tt>static balance get_balance(const_node_ptr n);</tt>
//!
//! <tt>static void set_balance(node_ptr n, balance b);</tt>
//!
//! <tt>static balance negative();</tt>
//!
//! <tt>static balance zero();</tt>
//!
//! <tt>static balance positive();</tt>
template<class NodeTraits>
class avltree_algorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public bstree_algorithms<NodeTraits>
   #endif
{
   public:
   typedef typename NodeTraits::node            node;
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;
   typedef typename NodeTraits::balance         balance;

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
      //Swap balance
      balance c = NodeTraits::get_balance(node1);
      NodeTraits::set_balance(node1, NodeTraits::get_balance(node2));
      NodeTraits::set_balance(node2, c);
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
      NodeTraits::set_balance(new_node, NodeTraits::get_balance(node_to_be_replaced));
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(const node_ptr&)
   static void unlink(const node_ptr & node)
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

   //! <b>Requires</b>: node must not be part of any tree.
   //!
   //! <b>Effects</b>: Initializes the header to represent an empty tree.
   //!   unique(header) == true.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If node is inserted in a tree, this function corrupts the tree.
   static void init_header(const node_ptr & header)
   {
      bstree_algo::init_header(header);
      NodeTraits::set_balance(header, NodeTraits::zero());
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(const node_ptr&,const node_ptr&)
   static node_ptr erase(const node_ptr & header, const node_ptr & z)
   {
      typename bstree_algo::data_for_rebalance info;
      bstree_algo::erase(header, z, avltree_erase_fixup<NodeTraits>(), info);
      //Rebalance avltree
      rebalance_after_erasure(header, info.x, info.x_parent);
      return z;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const const_node_ptr&,const node_ptr&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (const const_node_ptr & source_header, const node_ptr & target_header, Cloner cloner, Disposer disposer)
   {
      avltree_node_cloner<NodeTraits, Cloner> new_cloner(cloner);
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const const_node_ptr&,const KeyType&,KeyNodePtrCompare)
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(const node_ptr&,const node_ptr&,const insert_commit_data &)
   static void insert_unique_commit
      (const node_ptr & header, const node_ptr & new_value, const insert_commit_data &commit_data)
   {
      bstree_algo::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(header, new_value);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const const_node_ptr & p)
   {  return NodeTraits::get_balance(p) == NodeTraits::zero() && bstree_algo::is_header(p);  }


   /// @cond
   private:

   static void rebalance_after_erasure(const node_ptr & header, const node_ptr & xnode, const node_ptr & xnode_parent)
   {
      node_ptr x(xnode), x_parent(xnode_parent);
      for (node_ptr root = NodeTraits::get_parent(header); x != root; root = NodeTraits::get_parent(header)) {
         const balance x_parent_balance = NodeTraits::get_balance(x_parent);
         if(x_parent_balance == NodeTraits::zero()){
            NodeTraits::set_balance(x_parent,
               (x == NodeTraits::get_right(x_parent) ? NodeTraits::negative() : NodeTraits::positive()));
            break;       // the height didn't change, let's stop here
         }
         else if(x_parent_balance == NodeTraits::negative()){
            if (x == NodeTraits::get_left(x_parent)) {
               NodeTraits::set_balance(x_parent, NodeTraits::zero()); // balanced
               x = x_parent;
               x_parent = NodeTraits::get_parent(x_parent);
            }
            else {
               // x is right child
               // a is left child
               node_ptr a = NodeTraits::get_left(x_parent);
               BOOST_INTRUSIVE_INVARIANT_ASSERT(a);
               if (NodeTraits::get_balance(a) == NodeTraits::positive()) {
                  // a MUST have a right child
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_right(a));
                  rotate_left_right(x_parent, header);
                  x = NodeTraits::get_parent(x_parent);
                  x_parent = NodeTraits::get_parent(x);
               }
               else {
                  rotate_right(x_parent, header);
                  x = NodeTraits::get_parent(x_parent);
                  x_parent = NodeTraits::get_parent(x);
               }

               // if changed from negative to NodeTraits::positive(), no need to check above
               if (NodeTraits::get_balance(x) == NodeTraits::positive()){
                  break;
               }
            }
         }
         else if(x_parent_balance == NodeTraits::positive()){
            if (x == NodeTraits::get_right(x_parent)) {
               NodeTraits::set_balance(x_parent, NodeTraits::zero()); // balanced
               x = x_parent;
               x_parent = NodeTraits::get_parent(x_parent);
            }
            else {
               // x is left child
               // a is right child
               node_ptr a = NodeTraits::get_right(x_parent);
               BOOST_INTRUSIVE_INVARIANT_ASSERT(a);
               if (NodeTraits::get_balance(a) == NodeTraits::negative()) {
                  // a MUST have then a left child
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_left(a));
                  rotate_right_left(x_parent, header);

                  x = NodeTraits::get_parent(x_parent);
                  x_parent = NodeTraits::get_parent(x);
               }
               else {
                  rotate_left(x_parent, header);
                  x = NodeTraits::get_parent(x_parent);
                  x_parent = NodeTraits::get_parent(x);
               }
               // if changed from NodeTraits::positive() to negative, no need to check above
               if (NodeTraits::get_balance(x) == NodeTraits::negative()){
                  break;
               }
            }
         }
         else{
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);  // never reached
         }
      }
   }

   static void rebalance_after_insertion(const node_ptr & header, const node_ptr & xnode)
   {
      node_ptr x(xnode);
      NodeTraits::set_balance(x, NodeTraits::zero());
      // Rebalance.
      for(node_ptr root = NodeTraits::get_parent(header); x != root; root = NodeTraits::get_parent(header)){
         const balance x_parent_balance = NodeTraits::get_balance(NodeTraits::get_parent(x));

         if(x_parent_balance == NodeTraits::zero()){
            // if x is left, parent will have parent->bal_factor = negative
            // else, parent->bal_factor = NodeTraits::positive()
            NodeTraits::set_balance( NodeTraits::get_parent(x)
                                    , x == NodeTraits::get_left(NodeTraits::get_parent(x))
                                       ? NodeTraits::negative() : NodeTraits::positive()  );
            x = NodeTraits::get_parent(x);
         }
         else if(x_parent_balance == NodeTraits::positive()){
            // if x is a left child, parent->bal_factor = zero
            if (x == NodeTraits::get_left(NodeTraits::get_parent(x)))
               NodeTraits::set_balance(NodeTraits::get_parent(x), NodeTraits::zero());
            else{        // x is a right child, needs rebalancing
               if (NodeTraits::get_balance(x) == NodeTraits::negative())
                  rotate_right_left(NodeTraits::get_parent(x), header);
               else
                  rotate_left(NodeTraits::get_parent(x), header);
            }
            break;
         }
         else if(x_parent_balance == NodeTraits::negative()){
            // if x is a left child, needs rebalancing
            if (x == NodeTraits::get_left(NodeTraits::get_parent(x))) {
               if (NodeTraits::get_balance(x) == NodeTraits::positive())
                  rotate_left_right(NodeTraits::get_parent(x), header);
               else
                  rotate_right(NodeTraits::get_parent(x), header);
            }
            else
               NodeTraits::set_balance(NodeTraits::get_parent(x), NodeTraits::zero());
            break;
         }
         else{
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);  // never reached
         }
      }
   }

   static void left_right_balancing(const node_ptr & a, const node_ptr & b, const node_ptr & c)
   {
      // balancing...
      const balance c_balance = NodeTraits::get_balance(c);
      const balance zero_balance = NodeTraits::zero();
      NodeTraits::set_balance(c, zero_balance);
      if(c_balance == NodeTraits::negative()){
         NodeTraits::set_balance(a, NodeTraits::positive());
         NodeTraits::set_balance(b, zero_balance);
      }
      else if(c_balance == zero_balance){
         NodeTraits::set_balance(a, zero_balance);
         NodeTraits::set_balance(b, zero_balance);
      }
      else if(c_balance == NodeTraits::positive()){
         NodeTraits::set_balance(a, zero_balance);
         NodeTraits::set_balance(b, NodeTraits::negative());
      }
      else{
         BOOST_INTRUSIVE_INVARIANT_ASSERT(false); // never reached
      }
   }

   static void rotate_left_right(const node_ptr a, const node_ptr & hdr)
   {
      //             |                               |         //
      //             a(-2)                           c         //
      //            / \                             / \        //
      //           /   \        ==>                /   \       //
      //      (pos)b    [g]                       b     a      //
      //          / \                            / \   / \     //
      //        [d]  c                         [d]  e f  [g]   //
      //           / \                                         //
      //          e   f                                        //
      node_ptr b = NodeTraits::get_left(a), c = NodeTraits::get_right(b);
      bstree_algo::rotate_left(b, hdr);
      bstree_algo::rotate_right(a, hdr);
      left_right_balancing(a, b, c);
   }

   static void rotate_right_left(const node_ptr a, const node_ptr & hdr)
   {
      //              |                               |           //
      //              a(pos)                          c           //
      //             / \                             / \          //
      //            /   \                           /   \         //
      //          [d]   b(neg)         ==>         a     b        //
      //               / \                        / \   / \       //
      //              c  [g]                    [d] e  f  [g]     //
      //             / \                                          //
      //            e   f                                         //
      node_ptr b = NodeTraits::get_right(a), c = NodeTraits::get_left(b);
      bstree_algo::rotate_right(b, hdr);
      bstree_algo::rotate_left(a, hdr);
      left_right_balancing(b, a, c);
   }

   static void rotate_left(const node_ptr x, const node_ptr & hdr)
   {
      const node_ptr y = NodeTraits::get_right(x);
      bstree_algo::rotate_left(x, hdr);

      // reset the balancing factor
      if (NodeTraits::get_balance(y) == NodeTraits::positive()) {
         NodeTraits::set_balance(x, NodeTraits::zero());
         NodeTraits::set_balance(y, NodeTraits::zero());
      }
      else {        // this doesn't happen during insertions
         NodeTraits::set_balance(x, NodeTraits::positive());
         NodeTraits::set_balance(y, NodeTraits::negative());
      }
   }

   static void rotate_right(const node_ptr x, const node_ptr & hdr)
   {
      const node_ptr y = NodeTraits::get_left(x);
      bstree_algo::rotate_right(x, hdr);

      // reset the balancing factor
      if (NodeTraits::get_balance(y) == NodeTraits::negative()) {
         NodeTraits::set_balance(x, NodeTraits::zero());
         NodeTraits::set_balance(y, NodeTraits::zero());
      }
      else {        // this doesn't happen during insertions
         NodeTraits::set_balance(x, NodeTraits::negative());
         NodeTraits::set_balance(y, NodeTraits::positive());
      }
   }

   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<AvlTreeAlgorithms, NodeTraits>
{
   typedef avltree_algorithms<NodeTraits> type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
