/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007.
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
// The code has been modified and (supposely) improved by Ion Gaztanaga.
// Here is the header of the file used as base code:
//
//  splay_tree.h -- implementation of a STL complatible splay tree.
//  
//  Copyright (c) 2004 Ralf Mattethat
//
//  Permission to copy, use, modify, sell and distribute this software
//  is granted provided this copyright notice appears in all copies.
//  This software is provided "as is" without express or implied
//  warranty, and with no claim as to its suitability for any purpose.
//
//  Please send questions, comments, complaints, performance data, etc to  
//  ralf.mattethat@teknologisk.dk
//
//  Requirements for element type
//  * must be copy-constructible
//  * destructor must not throw exception
//
//    Methods marked with note A only throws an exception if the evaluation of the 
//    predicate throws an exception. If an exception is thrown the call has no 
//    effect on the containers state
//
//    Methods marked with note B only throws an exception if the coppy constructor
//    or assignment operator of the predicate throws an exception. If an exception 
//    is thrown the call has no effect on the containers state
//
//    iterators are only invalidated, if the element pointed to by the iterator 
//    is deleted. The same goes for element references
//

#ifndef BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <cstddef>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/detail/tree_algorithms.hpp>

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

//! splaytree_algorithms is configured with a NodeTraits class, which encapsulates the
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
{
   /// @cond
   private:
   typedef detail::tree_algorithms<NodeTraits>  tree_algorithms;
   /// @endcond

   public:
   typedef typename NodeTraits::node            node;
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename tree_algorithms::insert_commit_data insert_commit_data;

   /// @cond
   private:
   static node_ptr uncast(const const_node_ptr & ptr)
   {  return pointer_traits<node_ptr>::const_cast_from(ptr);  }
   /// @endcond

   public:
   static node_ptr begin_node(const const_node_ptr & header)
   {  return tree_algorithms::begin_node(header);   }

   static node_ptr end_node(const const_node_ptr & header)
   {  return tree_algorithms::end_node(header);   }

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

   static void unlink(const node_ptr & node)
   {  tree_algorithms::unlink(node);   }

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
   {  tree_algorithms::swap_nodes(node1, header1, node2, header2);   }

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
   {  tree_algorithms::replace_node(node_to_be_replaced, header, new_node);   }

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
   {  tree_algorithms::init_header(header);  }

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
   {  tree_algorithms::insert_unique_commit(header, new_value, commit_data);  }

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
      (const node_ptr & header, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {
      splay_down(header, key, comp);
      return tree_algorithms::insert_unique_check(header, key, comp, commit_data);
   }

   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const node_ptr & header, const node_ptr &hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data)
   {
      splay_down(header, key, comp);
      return tree_algorithms::insert_unique_check(header, hint, key, comp, commit_data);
   }

   static bool is_header(const const_node_ptr & p)
   {  return tree_algorithms::is_header(p);  }

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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      if(splay)
         splay_down(uncast(header), key, comp);
      node_ptr end = uncast(header);
      node_ptr y = lower_bound(header, key, comp, false);
      node_ptr r = (y == end || comp(key, y)) ? end : y;
      return r;
   }

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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //if(splay)
         //splay_down(uncast(header), key, comp);
      std::pair<node_ptr, node_ptr> ret =
         tree_algorithms::equal_range(header, key, comp);

      if(splay)
         splay_up(ret.first, uncast(header));
      return ret;
   }

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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //if(splay)
         //splay_down(uncast(header), key, comp);
      node_ptr y = tree_algorithms::lower_bound(header, key, comp);
      if(splay)
         splay_up(y, uncast(header));
      return y;
   }

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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp, bool splay = true)
   {
      //if(splay)
         //splay_down(uncast(header), key, comp);
      node_ptr y = tree_algorithms::upper_bound(header, key, comp);
      if(splay)
         splay_up(y, uncast(header));
      return y;
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
      splay_down(header, new_node, comp);
      return tree_algorithms::insert_equal(header, hint, new_node, comp);
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
      splay_up(new_node, header);
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
      splay_up(new_node, header);
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
      splay_up(new_node, header);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
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
      (const node_ptr & header, const node_ptr & new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return tree_algorithms::insert_equal_upper_bound(header, new_node, comp);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
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
      (const node_ptr & header, const node_ptr & new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return tree_algorithms::insert_equal_lower_bound(header, new_node, comp);
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
   {  tree_algorithms::clone(source_header, target_header, cloner, disposer);   }

   // delete node                        | complexity : constant        | exception : nothrow
   static void erase(const node_ptr & header, const node_ptr & z, bool splay = true)
   {
//      node_base* n = t->right;
//      if( t->left != node_ptr() ){
//         node_base* l = t->previous();
//         splay_up( l , t );
//         n = t->left;
//         n->right = t->right;
//         if( n->right != node_ptr() )
//            n->right->parent = n;
//      }
//
//      if( n != node_ptr() )
//         n->parent = t->parent;
//
//      if( t->parent->left == t )
//         t->parent->left = n;
//      else // must be ( t->parent->right == t )
//         t->parent->right = n;
//
//      if( data_->parent == t )
//         data_->parent = find_leftmost();
         //posibility 1
      if(splay && NodeTraits::get_left(z)){
         splay_up(prev_node(z), header);
      }
      /*
      //possibility 2
      if(splay && NodeTraits::get_left(z) != node_ptr() ){
         node_ptr l = NodeTraits::get_left(z);
         splay_up(l, header);
      }*//*
      if(splay && NodeTraits::get_left(z) != node_ptr() ){
         node_ptr l = prev_node(z);
         splay_up_impl(l, z);
      }*/
      /*
      //possibility 4
      if(splay){
         splay_up(z, header);
      }*/

      //if(splay)
         //splay_up(z, header);
      tree_algorithms::erase(header, z);
   }

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
         detail::splaydown_rollback<NodeTraits> rollback(&t, header, leftmost, rightmost);
         node_ptr null = header;
         node_ptr l = null;
         node_ptr r = null;

         for( ;; ){
            if(comp(key, t)){
               if(NodeTraits::get_left(t) == node_ptr() )
                  break;
               if(comp(key, NodeTraits::get_left(t))){
                  t = tree_algorithms::rotate_right(t);

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
                     t = tree_algorithms::rotate_left( t );

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

         assemble(t, l, r, null);
         rollback.release();
      }

      //t is the current root
      NodeTraits::set_parent(header, t);
      NodeTraits::set_parent(t, header);
      //Recover leftmost/rightmost pointers
      NodeTraits::set_left (header, leftmost);
      NodeTraits::set_right(header, rightmost);
      return t;
   }

   //! <b>Requires</b>: header must be the header of a tree.
   //! 
   //! <b>Effects</b>: Rebalances the tree.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear.
   static void rebalance(const node_ptr & header)
   {  tree_algorithms::rebalance(header); }

   //! <b>Requires</b>: old_root is a node of a tree.
   //! 
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear.
   static node_ptr rebalance_subtree(const node_ptr & old_root)
   {  return tree_algorithms::rebalance_subtree(old_root); }


   //! <b>Requires</b>: "n" must be a node inserted in a tree.
   //!
   //! <b>Effects</b>: Returns a pointer to the header node of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr get_header(const node_ptr & n)
   {  return tree_algorithms::get_header(n);   }

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
      bool g_is_header = is_header(g);
      
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

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
