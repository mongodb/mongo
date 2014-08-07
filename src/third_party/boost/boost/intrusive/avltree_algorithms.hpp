/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Daniel K. O. 2005.
// (C) Copyright Ion Gaztanaga 2007.
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
#include <boost/intrusive/detail/tree_algorithms.hpp>
#include <boost/intrusive/pointer_traits.hpp>


namespace boost {
namespace intrusive {

//! avltree_algorithms is configured with a NodeTraits class, which encapsulates the
//! information about the node to be manipulated. NodeTraits must support the
//! following interface:
//!
//! <b>Typedefs</b>:
//!
//! <tt>node</tt>: The type of the node that forms the circular list
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
{
   public:
   typedef typename NodeTraits::node            node;
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;
   typedef typename NodeTraits::balance         balance;

   /// @cond
   private:
   typedef detail::tree_algorithms<NodeTraits>  tree_algorithms;

   template<class F>
   struct avltree_node_cloner
      :  private detail::ebo_functor_holder<F>
   {
      typedef detail::ebo_functor_holder<F>                 base_t;

      avltree_node_cloner(F f)
         :  base_t(f)
      {}
      
      node_ptr operator()(const node_ptr &p)
      {
         node_ptr n = base_t::get()(p);
         NodeTraits::set_balance(n, NodeTraits::get_balance(p));
         return n;
      }
   };

   struct avltree_erase_fixup
   {
      void operator()(const node_ptr &to_erase, const node_ptr &successor)
      {  NodeTraits::set_balance(successor, NodeTraits::get_balance(to_erase));  }
   };

   static node_ptr uncast(const const_node_ptr & ptr)
   {  return pointer_traits<node_ptr>::const_cast_from(ptr);  }
   /// @endcond

   public:
   static node_ptr begin_node(const const_node_ptr & header)
   {  return tree_algorithms::begin_node(header);   }

   static node_ptr end_node(const const_node_ptr & header)
   {  return tree_algorithms::end_node(header);   }

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename tree_algorithms::insert_commit_data insert_commit_data;

   //! <b>Requires</b>: header1 and header2 must be the header nodes
   //!  of two trees.
   //! 
   //! <b>Effects</b>: Swaps two trees. After the function header1 will contain 
   //!   links to the second tree and header2 will have links to the first tree.
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   static void swap_tree(const node_ptr & header1, const node_ptr & header2)
   {  return tree_algorithms::swap_tree(header1, header2);  }

   //! <b>Requires</b>: node1 and node2 can't be header nodes
   //!  of two trees.
   //! 
   //! <b>Effects</b>: Swaps two nodes. After the function node1 will be inserted
   //!   in the position node2 before the function. node2 will be inserted in the
   //!   position node1 had before the function.
   //! 
   //! <b>Complexity</b>: Logarithmic. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   node1 and node2 are not equivalent according to the ordering rules.
   //!
   //!Experimental function
   static void swap_nodes(const node_ptr & node1, const node_ptr & node2)
   {
      if(node1 == node2)
         return;
   
      node_ptr header1(tree_algorithms::get_header(node1)), header2(tree_algorithms::get_header(node2));
      swap_nodes(node1, header1, node2, header2);
   }

   //! <b>Requires</b>: node1 and node2 can't be header nodes
   //!  of two trees with header header1 and header2.
   //! 
   //! <b>Effects</b>: Swaps two nodes. After the function node1 will be inserted
   //!   in the position node2 before the function. node2 will be inserted in the
   //!   position node1 had before the function.
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   node1 and node2 are not equivalent according to the ordering rules.
   //!
   //!Experimental function
   static void swap_nodes(const node_ptr & node1, const node_ptr & header1, const node_ptr & node2, const node_ptr & header2)
   {
      if(node1 == node2)   return;

      tree_algorithms::swap_nodes(node1, header1, node2, header2);
      //Swap balance
      balance c = NodeTraits::get_balance(node1);
      NodeTraits::set_balance(node1, NodeTraits::get_balance(node2)); 
      NodeTraits::set_balance(node2, c); 
   }

   //! <b>Requires</b>: node_to_be_replaced must be inserted in a tree
   //!   and new_node must not be inserted in a tree.
   //! 
   //! <b>Effects</b>: Replaces node_to_be_replaced in its position in the
   //!   tree with new_node. The tree does not need to be rebalanced
   //! 
   //! <b>Complexity</b>: Logarithmic. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   new_node is not equivalent to node_to_be_replaced according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing and comparison is needed.
   //!
   //!Experimental function
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & new_node)
   {
      if(node_to_be_replaced == new_node)
         return;
      replace_node(node_to_be_replaced, tree_algorithms::get_header(node_to_be_replaced), new_node);
   }

   //! <b>Requires</b>: node_to_be_replaced must be inserted in a tree
   //!   with header "header" and new_node must not be inserted in a tree.
   //! 
   //! <b>Effects</b>: Replaces node_to_be_replaced in its position in the
   //!   tree with new_node. The tree does not need to be rebalanced
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   new_node is not equivalent to node_to_be_replaced according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   //!
   //!Experimental function
   static void replace_node(const node_ptr & node_to_be_replaced, const node_ptr & header, const node_ptr & new_node)
   {
      tree_algorithms::replace_node(node_to_be_replaced, header, new_node);
      NodeTraits::set_balance(new_node, NodeTraits::get_balance(node_to_be_replaced)); 
   }

   //! <b>Requires</b>: node is a tree node but not the header.
   //! 
   //! <b>Effects</b>: Unlinks the node and rebalances the tree.
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static void unlink(const node_ptr & node)
   {
      node_ptr x = NodeTraits::get_parent(node);
      if(x){
         while(!is_header(x))
            x = NodeTraits::get_parent(x);
         erase(x, node);
      }
   }

   //! <b>Requires</b>: header is the header of a tree.
   //! 
   //! <b>Effects</b>: Unlinks the leftmost node from the tree, and
   //!   updates the header link to the new leftmost node.
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function breaks the tree and the tree can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the tree.
   static node_ptr unlink_leftmost_without_rebalance(const node_ptr & header)
   {  return tree_algorithms::unlink_leftmost_without_rebalance(header);   }

   //! <b>Requires</b>: node is a node of the tree or an node initialized
   //!   by init(...).
   //! 
   //! <b>Effects</b>: Returns true if the node is initialized by init().
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool unique(const const_node_ptr & node)
   {  return tree_algorithms::unique(node);  }

   //! <b>Requires</b>: node is a node of the tree but it's not the header.
   //! 
   //! <b>Effects</b>: Returns the number of nodes of the subtree.
   //! 
   //! <b>Complexity</b>: Linear time.
   //! 
   //! <b>Throws</b>: Nothing.
   static std::size_t count(const const_node_ptr & node)
   {  return tree_algorithms::count(node);   }

   //! <b>Requires</b>: header is the header node of the tree.
   //! 
   //! <b>Effects</b>: Returns the number of nodes above the header.
   //! 
   //! <b>Complexity</b>: Linear time.
   //! 
   //! <b>Throws</b>: Nothing.
   static std::size_t size(const const_node_ptr & header)
   {  return tree_algorithms::size(header);   }

   //! <b>Requires</b>: p is a node from the tree except the header.
   //! 
   //! <b>Effects</b>: Returns the next node of the tree.
   //! 
   //! <b>Complexity</b>: Average constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr next_node(const node_ptr & p)
   {  return tree_algorithms::next_node(p); }

   //! <b>Requires</b>: p is a node from the tree except the leftmost node.
   //! 
   //! <b>Effects</b>: Returns the previous node of the tree.
   //! 
   //! <b>Complexity</b>: Average constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr prev_node(const node_ptr & p)
   {  return tree_algorithms::prev_node(p); }

   //! <b>Requires</b>: node must not be part of any tree.
   //!
   //! <b>Effects</b>: After the function unique(node) == true.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If node is inserted in a tree, this function corrupts the tree.
   static void init(const node_ptr & node)
   {  tree_algorithms::init(node);  }

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
      tree_algorithms::init_header(header);
      NodeTraits::set_balance(header, NodeTraits::zero()); 
   }

   //! <b>Requires</b>: header must be the header of a tree, z a node
   //!    of that tree and z != header.
   //!
   //! <b>Effects</b>: Erases node "z" from the tree with header "header".
   //! 
   //! <b>Complexity</b>: Amortized constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr erase(const node_ptr & header, const node_ptr & z)
   {
      typename tree_algorithms::data_for_rebalance info;
      tree_algorithms::erase(header, z, avltree_erase_fixup(), info);
      node_ptr x = info.x;
      node_ptr x_parent = info.x_parent;

      //Rebalance avltree
      rebalance_after_erasure(header, x, x_parent);
      return z;
   }

   //! <b>Requires</b>: "cloner" must be a function
   //!   object taking a node_ptr and returning a new cloned node of it. "disposer" must
   //!   take a node_ptr and shouldn't throw.
   //!
   //! <b>Effects</b>: First empties target tree calling 
   //!   <tt>void disposer::operator()(const node_ptr &)</tt> for every node of the tree
   //!    except the header.
   //!    
   //!   Then, duplicates the entire tree pointed by "source_header" cloning each
   //!   source node with <tt>node_ptr Cloner::operator()(const node_ptr &)</tt> to obtain 
   //!   the nodes of the target tree. If "cloner" throws, the cloned target nodes
   //!   are disposed using <tt>void disposer(const node_ptr &)</tt>.
   //! 
   //! <b>Complexity</b>: Linear to the number of element of the source tree plus the.
   //!   number of elements of tree target tree when calling this function.
   //! 
   //! <b>Throws</b>: If cloner functor throws. If this happens target nodes are disposed.
   template <class Cloner, class Disposer>
   static void clone
      (const const_node_ptr & source_header, const node_ptr & target_header, Cloner cloner, Disposer disposer)
   {
      avltree_node_cloner<Cloner> new_cloner(cloner);
      tree_algorithms::clone(source_header, target_header, new_cloner, disposer);
   }

   //! <b>Requires</b>: "disposer" must be an object function
   //!   taking a node_ptr parameter and shouldn't throw.
   //!
   //! <b>Effects</b>: Empties the target tree calling 
   //!   <tt>void disposer::operator()(const node_ptr &)</tt> for every node of the tree
   //!    except the header.
   //! 
   //! <b>Complexity</b>: Linear to the number of element of the source tree plus the.
   //!   number of elements of tree target tree when calling this function.
   //! 
   //! <b>Throws</b>: If cloner functor throws. If this happens target nodes are disposed.
   template<class Disposer>
   static void clear_and_dispose(const node_ptr & header, Disposer disposer)
   {  tree_algorithms::clear_and_dispose(header, disposer); }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an node_ptr to the first element that is
   //!   not less than "key" according to "comp" or "header" if that element does
   //!   not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr lower_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_algorithms::lower_bound(header, key, comp);  }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an node_ptr to the first element that is greater
   //!   than "key" according to "comp" or "header" if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr upper_bound
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_algorithms::upper_bound(header, key, comp);  }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an node_ptr to the element that is equivalent to
   //!   "key" according to "comp" or "header" if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr find
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_algorithms::find(header, key, comp);  }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an a pair of node_ptr delimiting a range containing
   //!   all elements that are equivalent to "key" according to "comp" or an
   //!   empty range that indicates the position where those elements would be
   //!   if they there are no equivalent elements.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> equal_range
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_algorithms::equal_range(header, key, comp);  }

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the upper bound
   //!   according to "comp".
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static node_ptr insert_equal_upper_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp)
   {
      tree_algorithms::insert_equal_upper_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the lower bound
   //!   according to "comp".
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static node_ptr insert_equal_lower_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp)
   {
      tree_algorithms::insert_equal_lower_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs. "hint" is node from
   //!   the "header"'s tree.
   //!   
   //! <b>Effects</b>: Inserts new_node into the tree, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case).
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if new_node is inserted immediately before "hint".
   //! 
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static node_ptr insert_equal
      (const node_ptr & header, const node_ptr & hint, const node_ptr & new_node, NodePtrCompare comp)
   {
      tree_algorithms::insert_equal(header, hint, new_node, comp);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "pos" must be a valid iterator or header (end) node.
   //!   "pos" must be an iterator pointing to the successor to "new_node"
   //!   once inserted according to the order of already inserted nodes. This function does not
   //!   check "pos" and this precondition must be guaranteed by the caller.
   //!   
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: If "pos" is not the successor of the newly inserted "new_node"
   //! tree invariants might be broken.
   static node_ptr insert_before
      (const node_ptr & header, const node_ptr & pos, const node_ptr & new_node)
   {
      tree_algorithms::insert_before(header, pos, new_node);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering no less than the
   //!   greatest inserted key.
   //!   
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: If "new_node" is less than the greatest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   static void push_back(const node_ptr & header, const node_ptr & new_node)
   {
      tree_algorithms::push_back(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering, no greater than the
   //!   lowest inserted key.
   //!   
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: If "new_node" is greater than the lowest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   static void push_front(const node_ptr & header, const node_ptr & new_node)
   {
      tree_algorithms::push_front(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares KeyType with a node_ptr.
   //! 
   //! <b>Effects</b>: Checks if there is an equivalent node to "key" in the
   //!   tree according to "comp" and obtains the needed information to realize
   //!   a constant-time node insertion if there is no equivalent node.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing a node_ptr to the already present node
   //!   and false. If there is not equivalent key can be inserted returns true
   //!   in the returned pair's boolean and fills "commit_data" that is meant to
   //!   be used with the "insert_commit" function to achieve a constant-time
   //!   insertion function.
   //! 
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   //! 
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a node is expensive and the user does not want to have two equivalent nodes
   //!   in the tree: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the node and this function offers the possibility to use that part
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the node and use
   //!   "insert_commit" to insert the node in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_unique_commit" only
   //!   if no more objects are inserted or erased from the set.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const const_node_ptr & header,  const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {  return tree_algorithms::insert_unique_check(header, key, comp, commit_data);  }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares KeyType with a node_ptr.
   //!   "hint" is node from the "header"'s tree.
   //! 
   //! <b>Effects</b>: Checks if there is an equivalent node to "key" in the
   //!   tree according to "comp" using "hint" as a hint to where it should be
   //!   inserted and obtains the needed information to realize
   //!   a constant-time node insertion if there is no equivalent node. 
   //!   If "hint" is the upper_bound the function has constant time 
   //!   complexity (two comparisons in the worst case).
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing a node_ptr to the already present node
   //!   and false. If there is not equivalent key can be inserted returns true
   //!   in the returned pair's boolean and fills "commit_data" that is meant to
   //!   be used with the "insert_commit" function to achieve a constant-time
   //!   insertion function.
   //! 
   //! <b>Complexity</b>: Average complexity is at most logarithmic, but it is
   //!   amortized constant time if new_node should be inserted immediately before "hint".
   //!
   //! <b>Throws</b>: If "comp" throws.
   //! 
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a node is expensive and the user does not want to have two equivalent nodes
   //!   in the tree: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the node and this function offers the possibility to use that part
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the node and use
   //!   "insert_commit" to insert the node in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_unique_commit" only
   //!   if no more objects are inserted or erased from the set.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const const_node_ptr & header, const node_ptr &hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {  return tree_algorithms::insert_unique_check(header, hint, key, comp, commit_data);  }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "commit_data" must have been obtained from a previous call to
   //!   "insert_unique_check". No objects should have been inserted or erased
   //!   from the set between the "insert_unique_check" that filled "commit_data"
   //!   and the call to "insert_commit". 
   //! 
   //! 
   //! <b>Effects</b>: Inserts new_node in the set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function has only sense if a "insert_unique_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   static void insert_unique_commit
      (const node_ptr & header, const node_ptr & new_value, const insert_commit_data &commit_data)
   {
      tree_algorithms::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(header, new_value);
   }

   //! <b>Requires</b>: "n" must be a node inserted in a tree.
   //!
   //! <b>Effects</b>: Returns a pointer to the header node of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr get_header(const node_ptr & n)
   {  return tree_algorithms::get_header(n);   }

   /// @cond
   private:

   //! <b>Requires</b>: p is a node of a tree.
   //! 
   //! <b>Effects</b>: Returns true if p is the header of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool is_header(const const_node_ptr & p)
   {  return NodeTraits::get_balance(p) == NodeTraits::zero() && tree_algorithms::is_header(p);  }

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
      tree_algorithms::rotate_left(b, hdr);
      tree_algorithms::rotate_right(a, hdr);
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
      tree_algorithms::rotate_right(b, hdr);
      tree_algorithms::rotate_left(a, hdr);
      left_right_balancing(b, a, c);
   }

   static void rotate_left(const node_ptr x, const node_ptr & hdr)
   {
      const node_ptr y = NodeTraits::get_right(x);
      tree_algorithms::rotate_left(x, hdr);

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
      tree_algorithms::rotate_right(x, hdr);

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

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
