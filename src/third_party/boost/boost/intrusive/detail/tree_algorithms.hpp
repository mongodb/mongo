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

#ifndef BOOST_INTRUSIVE_TREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_TREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <cstddef>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost {
namespace intrusive {
namespace detail {

//!   This is an implementation of a binary search tree.
//!   A node in the search tree has references to its children and its parent. This 
//!   is to allow traversal of the whole tree from a given node making the 
//!   implementation of iterator a pointer to a node.
//!   At the top of the tree a node is used specially. This node's parent pointer 
//!   is pointing to the root of the tree. Its left pointer points to the 
//!   leftmost node in the tree and the right pointer to the rightmost one.
//!   This node is used to represent the end-iterator.
//!
//!                                            +---------+ 
//!       header------------------------------>|         | 
//!                                            |         | 
//!                   +----------(left)--------|         |--------(right)---------+ 
//!                   |                        +---------+                        | 
//!                   |                             |                             | 
//!                   |                             | (parent)                    |
//!                   |                             |                             |
//!                   |                             |                             |
//!                   |                        +---------+                        |
//!    root of tree ..|......................> |         |                        |
//!                   |                        |    D    |                        |
//!                   |                        |         |                        |
//!                   |                +-------+---------+-------+                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |          +---------+                 +---------+          |
//!                   |          |         |                 |         |          |
//!                   |          |    B    |                 |    F    |          |
//!                   |          |         |                 |         |          |
//!                   |       +--+---------+--+           +--+---------+--+       |
//!                   |       |               |           |               |       |
//!                   |       |               |           |               |       |
//!                   |       |               |           |               |       |
//!                   |   +---+-----+   +-----+---+   +---+-----+   +-----+---+   |
//!                   +-->|         |   |         |   |         |   |         |<--+ 
//!                       |    A    |   |    C    |   |    E    |   |    G    | 
//!                       |         |   |         |   |         |   |         | 
//!                       +---------+   +---------+   +---------+   +---------+ 
//!

//! tree_algorithms is configured with a NodeTraits class, which encapsulates the
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
class tree_algorithms
{
   public:
   typedef typename NodeTraits::node            node;
   typedef NodeTraits                           node_traits;
   typedef typename NodeTraits::node_ptr        node_ptr;
   typedef typename NodeTraits::const_node_ptr  const_node_ptr;

   //! This type is the information that will be filled by insert_unique_check
   struct insert_commit_data
   {
      insert_commit_data()
         :  link_left(false)
         ,  node()
      {}
      bool     link_left;
      node_ptr node;
   };

   struct nop_erase_fixup
   {
      void operator()(const node_ptr&, const node_ptr&){}
   };

   /// @cond
   private:
   template<class Disposer>
   struct dispose_subtree_disposer
   {
      dispose_subtree_disposer(Disposer &disp, const node_ptr & subtree)
         : disposer_(&disp), subtree_(subtree)
      {}

      void release()
      {  disposer_ = 0;  }

      ~dispose_subtree_disposer()
      {
         if(disposer_){
            dispose_subtree(subtree_, *disposer_);
         }
      }
      Disposer *disposer_;
      node_ptr subtree_;
   };

   static node_ptr uncast(const const_node_ptr & ptr)
   {  return pointer_traits<node_ptr>::const_cast_from(ptr);  }

   /// @endcond

   public:
   static node_ptr begin_node(const const_node_ptr & header)
   {  return node_traits::get_left(header);   }

   static node_ptr end_node(const const_node_ptr & header)
   {  return uncast(header);   }

   //! <b>Requires</b>: 'node' is a node of the tree or an node initialized
   //!   by init(...) or init_node.
   //! 
   //! <b>Effects</b>: Returns true if the node is initialized by init() or init_node().
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool unique(const const_node_ptr & node)
   { return !NodeTraits::get_parent(node); }

   static node_ptr get_header(const const_node_ptr & node)
   {
      node_ptr h = uncast(node);
      if(NodeTraits::get_parent(node)){
         h = NodeTraits::get_parent(node);
         while(!is_header(h))
            h = NodeTraits::get_parent(h);
      }
      return h;
   }

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
   
      node_ptr header1(get_header(node1)), header2(get_header(node2));
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
      if(node1 == node2)
         return;
   
      //node1 and node2 must not be header nodes 
      //BOOST_INTRUSIVE_INVARIANT_ASSERT((header1 != node1 && header2 != node2));
      if(header1 != header2){
         //Update header1 if necessary
         if(node1 == NodeTraits::get_left(header1)){
            NodeTraits::set_left(header1, node2);
         }

         if(node1 == NodeTraits::get_right(header1)){
            NodeTraits::set_right(header1, node2);
         }

         if(node1 == NodeTraits::get_parent(header1)){
            NodeTraits::set_parent(header1, node2);
         }

         //Update header2 if necessary
         if(node2 == NodeTraits::get_left(header2)){
            NodeTraits::set_left(header2, node1);
         }

         if(node2 == NodeTraits::get_right(header2)){
            NodeTraits::set_right(header2, node1);
         }

         if(node2 == NodeTraits::get_parent(header2)){
            NodeTraits::set_parent(header2, node1);
         }
      }
      else{
         //If both nodes are from the same tree
         //Update header if necessary
         if(node1 == NodeTraits::get_left(header1)){
            NodeTraits::set_left(header1, node2);
         }
         else if(node2 == NodeTraits::get_left(header2)){
            NodeTraits::set_left(header2, node1);
         }

         if(node1 == NodeTraits::get_right(header1)){
            NodeTraits::set_right(header1, node2);
         }
         else if(node2 == NodeTraits::get_right(header2)){
            NodeTraits::set_right(header2, node1);
         }

         if(node1 == NodeTraits::get_parent(header1)){
            NodeTraits::set_parent(header1, node2);
         }
         else if(node2 == NodeTraits::get_parent(header2)){
            NodeTraits::set_parent(header2, node1);
         }

         //Adjust data in nodes to be swapped
         //so that final link swap works as expected
         if(node1 == NodeTraits::get_parent(node2)){
            NodeTraits::set_parent(node2, node2);

            if(node2 == NodeTraits::get_right(node1)){
               NodeTraits::set_right(node1, node1);
            }
            else{
               NodeTraits::set_left(node1, node1);
            }
         }
         else if(node2 == NodeTraits::get_parent(node1)){
            NodeTraits::set_parent(node1, node1);

            if(node1 == NodeTraits::get_right(node2)){
               NodeTraits::set_right(node2, node2);
            }
            else{
               NodeTraits::set_left(node2, node2);
            }
         }
      }

      //Now swap all the links
      node_ptr temp;
      //swap left link
      temp = NodeTraits::get_left(node1);
      NodeTraits::set_left(node1, NodeTraits::get_left(node2));
      NodeTraits::set_left(node2, temp);
      //swap right link
      temp = NodeTraits::get_right(node1);
      NodeTraits::set_right(node1, NodeTraits::get_right(node2));
      NodeTraits::set_right(node2, temp);
      //swap parent link
      temp = NodeTraits::get_parent(node1);
      NodeTraits::set_parent(node1, NodeTraits::get_parent(node2));
      NodeTraits::set_parent(node2, temp);

      //Now adjust adjacent nodes for newly inserted node 1
      if((temp = NodeTraits::get_left(node1))){
         NodeTraits::set_parent(temp, node1);
      }
      if((temp = NodeTraits::get_right(node1))){
         NodeTraits::set_parent(temp, node1);
      }
      if((temp = NodeTraits::get_parent(node1)) &&
         //The header has been already updated so avoid it
         temp != header2){
         if(NodeTraits::get_left(temp) == node2){
            NodeTraits::set_left(temp, node1);
         }
         if(NodeTraits::get_right(temp) == node2){
            NodeTraits::set_right(temp, node1);
         }
      }
      //Now adjust adjacent nodes for newly inserted node 2
      if((temp = NodeTraits::get_left(node2))){
         NodeTraits::set_parent(temp, node2);
      }
      if((temp = NodeTraits::get_right(node2))){
         NodeTraits::set_parent(temp, node2);
      }
      if((temp = NodeTraits::get_parent(node2)) &&
         //The header has been already updated so avoid it
         temp != header1){
         if(NodeTraits::get_left(temp) == node1){
            NodeTraits::set_left(temp, node2);
         }
         if(NodeTraits::get_right(temp) == node1){
            NodeTraits::set_right(temp, node2);
         }
      }
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
      replace_node(node_to_be_replaced, get_header(node_to_be_replaced), new_node);
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
      if(node_to_be_replaced == new_node)
         return;
   
      //Update header if necessary
      if(node_to_be_replaced == NodeTraits::get_left(header)){
         NodeTraits::set_left(header, new_node);
      }

      if(node_to_be_replaced == NodeTraits::get_right(header)){
         NodeTraits::set_right(header, new_node);
      }

      if(node_to_be_replaced == NodeTraits::get_parent(header)){
         NodeTraits::set_parent(header, new_node);
      }

      //Now set data from the original node
      node_ptr temp;
      NodeTraits::set_left(new_node, NodeTraits::get_left(node_to_be_replaced));
      NodeTraits::set_right(new_node, NodeTraits::get_right(node_to_be_replaced));
      NodeTraits::set_parent(new_node, NodeTraits::get_parent(node_to_be_replaced));

      //Now adjust adjacent nodes for newly inserted node
      if((temp = NodeTraits::get_left(new_node))){
         NodeTraits::set_parent(temp, new_node);
      }
      if((temp = NodeTraits::get_right(new_node))){
         NodeTraits::set_parent(temp, new_node);
      }
      if((temp = NodeTraits::get_parent(new_node)) &&
         //The header has been already updated so avoid it
         temp != header){
         if(NodeTraits::get_left(temp) == node_to_be_replaced){
            NodeTraits::set_left(temp, new_node);
         }
         if(NodeTraits::get_right(temp) == node_to_be_replaced){
            NodeTraits::set_right(temp, new_node);
         }
      }
   }

   //! <b>Requires</b>: 'node' is a node from the tree except the header.
   //! 
   //! <b>Effects</b>: Returns the next node of the tree.
   //! 
   //! <b>Complexity</b>: Average constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr next_node(const node_ptr & node)
   {
      node_ptr p_right(NodeTraits::get_right(node));
      if(p_right){
         return minimum(p_right);
      }
      else {
         node_ptr p(node);
         node_ptr x = NodeTraits::get_parent(p);
         while(p == NodeTraits::get_right(x)){
            p = x;
            x = NodeTraits::get_parent(x);
         }
         return NodeTraits::get_right(p) != x ? x : uncast(p);
      }
   }

   //! <b>Requires</b>: 'node' is a node from the tree except the leftmost node.
   //! 
   //! <b>Effects</b>: Returns the previous node of the tree.
   //! 
   //! <b>Complexity</b>: Average constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr prev_node(const node_ptr & node)
   {
      if(is_header(node)){
         return NodeTraits::get_right(node);
         //return maximum(NodeTraits::get_parent(node));
      }
      else if(NodeTraits::get_left(node)){
         return maximum(NodeTraits::get_left(node));
      }
      else {
         node_ptr p(node);
         node_ptr x = NodeTraits::get_parent(p);
         while(p == NodeTraits::get_left(x)){
            p = x;
            x = NodeTraits::get_parent(x);
         }
         return x;
      }
   }

   //! <b>Requires</b>: 'node' is a node of a tree but not the header.
   //! 
   //! <b>Effects</b>: Returns the minimum node of the subtree starting at p.
   //! 
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr minimum (const node_ptr & node)
   {
      node_ptr p(node);
      for(node_ptr p_left = NodeTraits::get_left(p)
         ;p_left
         ;p_left = NodeTraits::get_left(p)){
         p = p_left;
      }
      return p;
   }

   //! <b>Requires</b>: 'node' is a node of a tree but not the header.
   //! 
   //! <b>Effects</b>: Returns the maximum node of the subtree starting at p.
   //! 
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr maximum(const node_ptr & node)
   {
      node_ptr p(node);
      for(node_ptr p_right = NodeTraits::get_right(p)
         ;p_right
         ;p_right = NodeTraits::get_right(p)){
         p = p_right;
      }
      return p;
   }

   //! <b>Requires</b>: 'node' must not be part of any tree.
   //!
   //! <b>Effects</b>: After the function unique(node) == true.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If node is inserted in a tree, this function corrupts the tree.
   static void init(const node_ptr & node)
   {
      NodeTraits::set_parent(node, node_ptr());
      NodeTraits::set_left(node, node_ptr());
      NodeTraits::set_right(node, node_ptr()); 
   };

   //! <b>Effects</b>: Returns true if node is in the same state as if called init(node)
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool inited(const const_node_ptr & node)
   {
      return !NodeTraits::get_parent(node) && 
             !NodeTraits::get_left(node)   &&
             !NodeTraits::get_right(node)  ;
   };

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
      NodeTraits::set_parent(header, node_ptr());
      NodeTraits::set_left(header, header);
      NodeTraits::set_right(header, header); 
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
   {
      node_ptr source_root = NodeTraits::get_parent(header);
      if(!source_root)
         return;
      dispose_subtree(source_root, disposer);
      init_header(header);
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
   {
      node_ptr leftmost = NodeTraits::get_left(header);
      if (leftmost == header)
         return node_ptr();
      node_ptr leftmost_parent(NodeTraits::get_parent(leftmost));
      node_ptr leftmost_right (NodeTraits::get_right(leftmost));
      bool is_root = leftmost_parent == header;

      if (leftmost_right){
         NodeTraits::set_parent(leftmost_right, leftmost_parent);
         NodeTraits::set_left(header, tree_algorithms::minimum(leftmost_right));

         if (is_root)
            NodeTraits::set_parent(header, leftmost_right);
         else
            NodeTraits::set_left(NodeTraits::get_parent(header), leftmost_right);
      }
      else if (is_root){
         NodeTraits::set_parent(header, node_ptr());
         NodeTraits::set_left(header,  header);
         NodeTraits::set_right(header, header);
      }
      else{
         NodeTraits::set_left(leftmost_parent, node_ptr());
         NodeTraits::set_left(header, leftmost_parent);
      }
      return leftmost;
   }

   //! <b>Requires</b>: node is a node of the tree but it's not the header.
   //! 
   //! <b>Effects</b>: Returns the number of nodes of the subtree.
   //! 
   //! <b>Complexity</b>: Linear time.
   //! 
   //! <b>Throws</b>: Nothing.
   static std::size_t count(const const_node_ptr & subtree)
   {
      if(!subtree) return 0;
      std::size_t count = 0;
      node_ptr p = minimum(uncast(subtree));
      bool continue_looping = true;
      while(continue_looping){
         ++count;
         node_ptr p_right(NodeTraits::get_right(p));
         if(p_right){
            p = minimum(p_right);
         }
         else {
            for(;;){
               node_ptr q;
               if (p == subtree){
                  continue_looping = false;
                  break;
               }
               q = p;
               p = NodeTraits::get_parent(p);
               if (NodeTraits::get_left(p) == q)
                  break;
            }
         }
      }
      return count;
   }

   //! <b>Requires</b>: node is a node of the tree but it's not the header.
   //! 
   //! <b>Effects</b>: Returns the number of nodes of the subtree.
   //! 
   //! <b>Complexity</b>: Linear time.
   //! 
   //! <b>Throws</b>: Nothing.
   static std::size_t size(const const_node_ptr & header)
   {
      node_ptr beg(begin_node(header));
      node_ptr end(end_node(header));
      std::size_t i = 0;
      for(;beg != end; beg = next_node(beg)) ++i;
      return i;
   }

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
   {
      if(header1 == header2)
         return;
   
      node_ptr tmp;

      //Parent swap
      tmp = NodeTraits::get_parent(header1);
      NodeTraits::set_parent(header1, NodeTraits::get_parent(header2));
      NodeTraits::set_parent(header2, tmp);
      //Left swap
      tmp = NodeTraits::get_left(header1);
      NodeTraits::set_left(header1, NodeTraits::get_left(header2));
      NodeTraits::set_left(header2, tmp);
      //Right swap
      tmp = NodeTraits::get_right(header1);
      NodeTraits::set_right(header1, NodeTraits::get_right(header2));
      NodeTraits::set_right(header2, tmp);

      //Now test parent
      node_ptr h1_parent(NodeTraits::get_parent(header1));
      if(h1_parent){
         NodeTraits::set_parent(h1_parent, header1);
      }
      else{
         NodeTraits::set_left(header1, header1);
         NodeTraits::set_right(header1, header1);
      }

      node_ptr h2_parent(NodeTraits::get_parent(header2));
      if(h2_parent){
         NodeTraits::set_parent(h2_parent, header2);
      }
      else{
         NodeTraits::set_left(header2, header2);
         NodeTraits::set_right(header2, header2);
      }
   }

   static bool is_header(const const_node_ptr & p)
   {
      node_ptr p_left (NodeTraits::get_left(p));
      node_ptr p_right(NodeTraits::get_right(p));
      if(!NodeTraits::get_parent(p) || //Header condition when empty tree
         (p_left && p_right &&         //Header always has leftmost and rightmost
            (p_left == p_right ||      //Header condition when only node
               (NodeTraits::get_parent(p_left)  != p ||
                NodeTraits::get_parent(p_right) != p ))
               //When tree size > 1 headers can't be leftmost's
               //and rightmost's parent 
          )){
         return true;
      }
      return false;
   }

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
   {
      node_ptr end = uncast(header);
      node_ptr y = lower_bound(header, key, comp);
      return (y == end || comp(key, y)) ? end : y;
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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {
      node_ptr y = uncast(header);
      node_ptr x = NodeTraits::get_parent(header);

      while(x){
         if(comp(x, key)){
            x = NodeTraits::get_right(x);
         }
         else if(comp(key, x)){
            y = x;
            x = NodeTraits::get_left(x);
         }
         else{
            node_ptr xu(x), yu(y);
            y = x, x = NodeTraits::get_left(x);
            xu = NodeTraits::get_right(xu);

            while(x){
               if(comp(x, key)){
                  x = NodeTraits::get_right(x);
               }
               else {
                  y = x;
                  x = NodeTraits::get_left(x);
               }
            }

            while(xu){
               if(comp(key, xu)){
                  yu = xu;
                  xu = NodeTraits::get_left(xu);
               }
               else {
                  xu = NodeTraits::get_right(xu);
               }
            }
            return std::pair<node_ptr,node_ptr> (y, yu);
         }
      }
      return std::pair<node_ptr,node_ptr> (y, y);
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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {
      node_ptr y = uncast(header);
      node_ptr x = NodeTraits::get_parent(header);
      while(x){
         if(comp(x, key)){
            x = NodeTraits::get_right(x);
         }
         else {
            y = x;
            x = NodeTraits::get_left(x);
         }
      }
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
      (const const_node_ptr & header, const KeyType &key, KeyNodePtrCompare comp)
   {
      node_ptr y = uncast(header);
      node_ptr x = NodeTraits::get_parent(header);
      while(x){
         if(comp(key, x)){
            y = x;
            x = NodeTraits::get_left(x);
         }
         else {
            x = NodeTraits::get_right(x);
         }
      }
      return y;
   }

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
   {  return insert_commit(header, new_value, commit_data); }

   static void insert_commit
      (const node_ptr & header, const node_ptr & new_node, const insert_commit_data &commit_data)
   {
      //Check if commit_data has not been initialized by a insert_unique_check call.
      BOOST_INTRUSIVE_INVARIANT_ASSERT(commit_data.node != node_ptr());
      node_ptr parent_node(commit_data.node);
      if(parent_node == header){
         NodeTraits::set_parent(header, new_node);
         NodeTraits::set_right(header, new_node);
         NodeTraits::set_left(header, new_node);
      }
      else if(commit_data.link_left){
         NodeTraits::set_left(parent_node, new_node);
         if(parent_node == NodeTraits::get_left(header))
             NodeTraits::set_left(header, new_node);
      }
      else{
         NodeTraits::set_right(parent_node, new_node);
         if(parent_node == NodeTraits::get_right(header))
             NodeTraits::set_right(header, new_node);
      }
      NodeTraits::set_parent(new_node, parent_node);
      NodeTraits::set_right(new_node, node_ptr());
      NodeTraits::set_left(new_node, node_ptr());
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
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      std::size_t depth = 0;
      node_ptr h(uncast(header));
      node_ptr y(h);
      node_ptr x(NodeTraits::get_parent(y));
      node_ptr prev = node_ptr();

      //Find the upper bound, cache the previous value and if we should
      //store it in the left or right node
      bool left_child = true;
      while(x){
         ++depth;
         y = x;
         x = (left_child = comp(key, x)) ? 
               NodeTraits::get_left(x) : (prev = y, NodeTraits::get_right(x));
      }

      if(pdepth)  *pdepth = depth;

      //Since we've found the upper bound there is no other value with the same key if:
      //    - There is no previous node
      //    - The previous node is less than the key
      if(!prev || comp(prev, key)){
         commit_data.link_left = left_child;
         commit_data.node      = y;
         return std::pair<node_ptr, bool>(node_ptr(), true);
      }
      //If the previous value was not less than key, it means that it's equal
      //(because we've checked the upper bound)
      else{
         return std::pair<node_ptr, bool>(prev, false);
      }
   }

   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const const_node_ptr & header, const node_ptr &hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      //hint must be bigger than the key
      if(hint == header || comp(key, hint)){
         node_ptr prev(hint);
         //Previous value should be less than the key
         if(hint == begin_node(header)|| comp((prev = prev_node(hint)), key)){
            commit_data.link_left = unique(header) || !NodeTraits::get_left(hint);
            commit_data.node      = commit_data.link_left ? hint : prev;
            if(pdepth){
               *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
            }
            return std::pair<node_ptr, bool>(node_ptr(), true);
         }
      }
      //Hint was wrong, use hintless insertion
      return insert_unique_check(header, key, comp, commit_data, pdepth);
   }

   template<class NodePtrCompare>
   static void insert_equal_check
      (const node_ptr &header, const node_ptr & hint, const node_ptr & new_node, NodePtrCompare comp
      , insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      if(hint == header || !comp(hint, new_node)){
         node_ptr prev(hint);
         if(hint == NodeTraits::get_left(header) || 
            !comp(new_node, (prev = prev_node(hint)))){
            bool link_left = unique(header) || !NodeTraits::get_left(hint);
            commit_data.link_left = link_left;
            commit_data.node = link_left ? hint : prev;
            if(pdepth){
               *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
            }
         }
         else{
            insert_equal_upper_bound_check(header, new_node, comp, commit_data, pdepth);
         }
      }
      else{
         insert_equal_lower_bound_check(header, new_node, comp, commit_data, pdepth);
      }
   }

   template<class NodePtrCompare>
   static void insert_equal_upper_bound_check
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp, insert_commit_data & commit_data, std::size_t *pdepth = 0)
   {  insert_equal_check_impl(true, h, new_node, comp, commit_data, pdepth);  }

   template<class NodePtrCompare>
   static void insert_equal_lower_bound_check
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp, insert_commit_data & commit_data, std::size_t *pdepth = 0)
   {  insert_equal_check_impl(false, h, new_node, comp, commit_data, pdepth);  }

   template<class NodePtrCompare>
   static node_ptr insert_equal
      (const node_ptr & h, const node_ptr & hint, const node_ptr & new_node, NodePtrCompare comp, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      insert_equal_check(h, hint, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   template<class NodePtrCompare>
   static node_ptr insert_equal_upper_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      insert_equal_upper_bound_check(h, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   template<class NodePtrCompare>
   static node_ptr insert_equal_lower_bound
      (const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      insert_equal_lower_bound_check(h, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   static node_ptr insert_before
      (const node_ptr & header, const node_ptr & pos, const node_ptr & new_node, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      insert_before_check(header, pos, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
      return new_node;
   }

   static void insert_before_check
      (const node_ptr &header, const node_ptr & pos
      , insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      node_ptr prev(pos);
      if(pos != NodeTraits::get_left(header))
         prev = prev_node(pos);
      bool link_left = unique(header) || !NodeTraits::get_left(pos);
      commit_data.link_left = link_left;
      commit_data.node = link_left ? pos : prev;
      if(pdepth){
         *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
      }
   }

   static void push_back
      (const node_ptr & header, const node_ptr & new_node, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      push_back_check(header, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
   }

   static void push_back_check
      (const node_ptr & header, insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      node_ptr prev(NodeTraits::get_right(header));
      if(pdepth){
         *pdepth = prev == header ? 0 : depth(prev) + 1;
      }
      commit_data.link_left = false;
      commit_data.node = prev;
   }

   static void push_front
      (const node_ptr & header, const node_ptr & new_node, std::size_t *pdepth = 0)
   {
      insert_commit_data commit_data;
      push_front_check(header, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
   }

   static void push_front_check
      (const node_ptr & header, insert_commit_data &commit_data, std::size_t *pdepth = 0)
   {
      node_ptr pos(NodeTraits::get_left(header));
      if(pdepth){
         *pdepth = pos == header ? 0 : depth(pos) + 1;
      }
      commit_data.link_left = true;
      commit_data.node = pos;
   }

   //! <b>Requires</b>: 'node' can't be a header node.
   //! 
   //! <b>Effects</b>: Calculates the depth of a node: the depth of a
   //! node is the length (number of edges) of the path from the root
   //! to that node. (The root node is at depth 0.)
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of nodes in the tree. 
   //! 
   //! <b>Throws</b>: Nothing.
   static std::size_t depth(const const_node_ptr & node)
   {
      const_node_ptr p(node);
      std::size_t depth = 0;
      node_ptr p_parent;
      while(p != NodeTraits::get_parent(p_parent = NodeTraits::get_parent(p))){
         ++depth;
         p = p_parent;
      }
      return depth;
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
      if(!unique(target_header)){
         clear_and_dispose(target_header, disposer);
      }

      node_ptr leftmost, rightmost;
      node_ptr new_root = clone_subtree
         (source_header, target_header, cloner, disposer, leftmost, rightmost);

      //Now update header node
      NodeTraits::set_parent(target_header, new_root);
      NodeTraits::set_left  (target_header, leftmost);
      NodeTraits::set_right (target_header, rightmost);
   }

   template <class Cloner, class Disposer>
   static node_ptr clone_subtree
      (const const_node_ptr &source_parent, const node_ptr &target_parent
      , Cloner cloner, Disposer disposer
      , node_ptr &leftmost_out, node_ptr &rightmost_out
      )
   {
      node_ptr target_sub_root = target_parent;
      node_ptr source_root = NodeTraits::get_parent(source_parent);
      if(!source_root){
         leftmost_out = rightmost_out = source_root;
      }
      else{
         //We'll calculate leftmost and rightmost nodes while iterating
         node_ptr current = source_root;
         node_ptr insertion_point = target_sub_root = cloner(current);

         //We'll calculate leftmost and rightmost nodes while iterating
         node_ptr leftmost  = target_sub_root;
         node_ptr rightmost = target_sub_root;

         //First set the subroot
         NodeTraits::set_left(target_sub_root, node_ptr());
         NodeTraits::set_right(target_sub_root, node_ptr());
         NodeTraits::set_parent(target_sub_root, target_parent);

         dispose_subtree_disposer<Disposer> rollback(disposer, target_sub_root);
         while(true) {
            //First clone left nodes
            if( NodeTraits::get_left(current) &&
               !NodeTraits::get_left(insertion_point)) {
               current = NodeTraits::get_left(current);
               node_ptr temp = insertion_point;
               //Clone and mark as leaf
               insertion_point = cloner(current);
               NodeTraits::set_left  (insertion_point, node_ptr());
               NodeTraits::set_right (insertion_point, node_ptr());
               //Insert left
               NodeTraits::set_parent(insertion_point, temp);
               NodeTraits::set_left  (temp, insertion_point);
               //Update leftmost
               if(rightmost == target_sub_root)
                  leftmost = insertion_point;
            }
            //Then clone right nodes
            else if( NodeTraits::get_right(current) && 
                     !NodeTraits::get_right(insertion_point)){
               current = NodeTraits::get_right(current);
               node_ptr temp = insertion_point;
               //Clone and mark as leaf
               insertion_point = cloner(current);
               NodeTraits::set_left  (insertion_point, node_ptr());
               NodeTraits::set_right (insertion_point, node_ptr());
               //Insert right
               NodeTraits::set_parent(insertion_point, temp);
               NodeTraits::set_right (temp, insertion_point);
               //Update rightmost
               rightmost = insertion_point;
            }
            //If not, go up
            else if(current == source_root){
               break;
            }
            else{
               //Branch completed, go up searching more nodes to clone
               current = NodeTraits::get_parent(current);
               insertion_point = NodeTraits::get_parent(insertion_point);
            }
         }
         rollback.release();
         leftmost_out   = leftmost;
         rightmost_out  = rightmost;
      }
      return target_sub_root;
   }

   template<class Disposer>
   static void dispose_subtree(const node_ptr & node, Disposer disposer)
   {
      node_ptr save;
      node_ptr x(node);
      while (x){
         save = NodeTraits::get_left(x);
         if (save) {
            // Right rotation
            NodeTraits::set_left(x, NodeTraits::get_right(save));
            NodeTraits::set_right(save, x);
         }
         else {
            save = NodeTraits::get_right(x);
            init(x);
            disposer(x);
         }
         x = save;
      }
   }

   //! <b>Requires</b>: p is a node of a tree.
   //! 
   //! <b>Effects</b>: Returns true if p is a left child.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool is_left_child(const node_ptr & p)
   {  return NodeTraits::get_left(NodeTraits::get_parent(p)) == p;  }

   //! <b>Requires</b>: p is a node of a tree.
   //! 
   //! <b>Effects</b>: Returns true if p is a right child.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   static bool is_right_child(const node_ptr & p)
   {  return NodeTraits::get_right(NodeTraits::get_parent(p)) == p;  }

   //Fix header and own's parent data when replacing x with own, providing own's old data with parent
   static void replace_own_impl(const node_ptr & own, const node_ptr & x, const node_ptr & header, const node_ptr & own_parent, bool own_was_left)
   {
      if(NodeTraits::get_parent(header) == own)
         NodeTraits::set_parent(header, x);
      else if(own_was_left)
         NodeTraits::set_left(own_parent, x);
      else
         NodeTraits::set_right(own_parent, x);
   }

   //Fix header and own's parent data when replacing x with own, supposing own
   //links with its parent are still ok
   static void replace_own(const node_ptr & own, const node_ptr & x, const node_ptr & header)
   {
      node_ptr own_parent(NodeTraits::get_parent(own));
      bool own_is_left(NodeTraits::get_left(own_parent) == own);
      replace_own_impl(own, x, header, own_parent, own_is_left);
   }

   // rotate parent p to left (no header and p's parent fixup)
   static node_ptr rotate_left(const node_ptr & p)
   {
      node_ptr x(NodeTraits::get_right(p));
      node_ptr x_left(NodeTraits::get_left(x));
      NodeTraits::set_right(p, x_left);
      if(x_left){
         NodeTraits::set_parent(x_left, p);
      }
      NodeTraits::set_left(x, p);
      NodeTraits::set_parent(p, x);
      return x;
   }

   // rotate parent p to left (with header and p's parent fixup)
   static void rotate_left(const node_ptr & p, const node_ptr & header)
   {
      bool     p_was_left(is_left_child(p));
      node_ptr p_old_parent(NodeTraits::get_parent(p));
      node_ptr x(rotate_left(p));
      NodeTraits::set_parent(x, p_old_parent);
      replace_own_impl(p, x, header, p_old_parent, p_was_left);
   }

   // rotate parent p to right (no header and p's parent fixup)
   static node_ptr rotate_right(const node_ptr & p)
   {
      node_ptr x(NodeTraits::get_left(p));
      node_ptr x_right(NodeTraits::get_right(x));
      NodeTraits::set_left(p, x_right);
      if(x_right){
         NodeTraits::set_parent(x_right, p);
      }
      NodeTraits::set_right(x, p);
      NodeTraits::set_parent(p, x);
      return x;
   }

   // rotate parent p to right (with header and p's parent fixup)
   static void rotate_right(const node_ptr & p, const node_ptr & header)
   {
      bool     p_was_left(is_left_child(p));
      node_ptr p_old_parent(NodeTraits::get_parent(p));
      node_ptr x(rotate_right(p));
      NodeTraits::set_parent(x, p_old_parent);
      replace_own_impl(p, x, header, p_old_parent, p_was_left);
   }

   static void erase(const node_ptr & header, const node_ptr & z)
   {
      data_for_rebalance ignored;
      erase_impl(header, z, ignored);
   }

   struct data_for_rebalance
   {
      node_ptr x;
      node_ptr x_parent;
      node_ptr y;
   };

   template<class F>
   static void erase(const node_ptr & header, const node_ptr & z, F z_and_successor_fixup, data_for_rebalance &info)
   {
      erase_impl(header, z, info);
      if(info.y != z){
         z_and_successor_fixup(z, info.y);
      }
   }

   static void unlink(const node_ptr & node)
   {
      node_ptr x = NodeTraits::get_parent(node);
      if(x){
         while(!is_header(x))
            x = NodeTraits::get_parent(x);
         erase(x, node);
      }
   }

   static void tree_to_vine(const node_ptr & header)
   {  subtree_to_vine(NodeTraits::get_parent(header)); }

   static void vine_to_tree(const node_ptr & header, std::size_t count)
   {  vine_to_subtree(NodeTraits::get_parent(header), count);  }

   static void rebalance(const node_ptr & header)
   {
      //Taken from:
      //"Tree rebalancing in optimal time and space"
      //Quentin F. Stout and Bette L. Warren
      std::size_t len = 0;
      subtree_to_vine(NodeTraits::get_parent(header), &len);
      vine_to_subtree(NodeTraits::get_parent(header), len);
   }

   static node_ptr rebalance_subtree(const node_ptr & old_root)
   {
      std::size_t len = 0;
      node_ptr new_root = subtree_to_vine(old_root, &len);
      return vine_to_subtree(new_root, len);
   }

   static node_ptr subtree_to_vine(const node_ptr & old_root, std::size_t *plen = 0)
   {
      std::size_t len;
      len = 0;
      if(!old_root)   return node_ptr();

      //To avoid irregularities in the algorithm (old_root can be a
      //left or right child or even the root of the tree) just put the 
      //root as the right child of its parent. Before doing this backup
      //information to restore the original relationship after
      //the algorithm is applied.
      node_ptr super_root = NodeTraits::get_parent(old_root);
      BOOST_INTRUSIVE_INVARIANT_ASSERT(super_root);

      //Get info
      node_ptr super_root_right_backup = NodeTraits::get_right(super_root);
      bool super_root_is_header   = is_header(super_root);
      bool old_root_is_right  = is_right_child(old_root);

      node_ptr x(old_root);
      node_ptr new_root(x);
      node_ptr save;
      bool moved_to_right = false;
      for( ; x; x = save){
         save = NodeTraits::get_left(x);
         if(save){
            // Right rotation
            node_ptr save_right = NodeTraits::get_right(save);
            node_ptr x_parent   = NodeTraits::get_parent(x);
            NodeTraits::set_parent(save, x_parent);
            NodeTraits::set_right (x_parent, save);
            NodeTraits::set_parent(x, save);
            NodeTraits::set_right (save, x);
            NodeTraits::set_left(x, save_right);
            if(save_right)
               NodeTraits::set_parent(save_right, x);
            if(!moved_to_right)
               new_root = save;
         }
         else{
            moved_to_right = true;
            save = NodeTraits::get_right(x);
            ++len;
         }
      }

      if(super_root_is_header){
         NodeTraits::set_right(super_root, super_root_right_backup);
         NodeTraits::set_parent(super_root, new_root);
      }
      else if(old_root_is_right){
         NodeTraits::set_right(super_root, new_root);
      }
      else{
         NodeTraits::set_right(super_root, super_root_right_backup);
         NodeTraits::set_left(super_root, new_root);
      }
      if(plen) *plen = len;
      return new_root;
   }

   static node_ptr vine_to_subtree(const node_ptr & old_root, std::size_t count)
   {
      std::size_t leaf_nodes = count + 1 - ((std::size_t) 1 << floor_log2 (count + 1));
      std::size_t vine_nodes = count - leaf_nodes;

      node_ptr new_root = compress_subtree(old_root, leaf_nodes);
      while(vine_nodes > 1){
         vine_nodes /= 2;
         new_root = compress_subtree(new_root, vine_nodes);
      }
      return new_root;
   }

   static node_ptr compress_subtree(const node_ptr & old_root, std::size_t count)
   {
      if(!old_root)   return old_root;

      //To avoid irregularities in the algorithm (old_root can be
      //left or right child or even the root of the tree) just put the 
      //root as the right child of its parent. First obtain
      //information to restore the original relationship after
      //the algorithm is applied.
      node_ptr super_root = NodeTraits::get_parent(old_root);
      BOOST_INTRUSIVE_INVARIANT_ASSERT(super_root);

      //Get info
      node_ptr super_root_right_backup = NodeTraits::get_right(super_root);
      bool super_root_is_header   = is_header(super_root);
      bool old_root_is_right  = is_right_child(old_root);

      //Put old_root as right child
      NodeTraits::set_right(super_root, old_root);

      //Start the compression algorithm            
      node_ptr even_parent = super_root;
      node_ptr new_root = old_root;

      while(count--){
         node_ptr even = NodeTraits::get_right(even_parent);
         node_ptr odd = NodeTraits::get_right(even);

         if(new_root == old_root)
            new_root = odd;

         node_ptr even_right = NodeTraits::get_left(odd);
         NodeTraits::set_right(even, even_right);
         if (even_right)
            NodeTraits::set_parent(even_right, even);

         NodeTraits::set_right(even_parent, odd);
         NodeTraits::set_parent(odd, even_parent);
         NodeTraits::set_left(odd, even);
         NodeTraits::set_parent(even, odd);
         even_parent = odd;
      }

      if(super_root_is_header){
         NodeTraits::set_parent(super_root, new_root);
         NodeTraits::set_right(super_root, super_root_right_backup);
      }
      else if(old_root_is_right){
         NodeTraits::set_right(super_root, new_root);
      }
      else{
         NodeTraits::set_left(super_root, new_root);
         NodeTraits::set_right(super_root, super_root_right_backup);
      }
      return new_root;
   }

   //! <b>Requires</b>: "n" must be a node inserted in a tree.
   //!
   //! <b>Effects</b>: Returns a pointer to the header node of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   static node_ptr get_root(const node_ptr & node)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT((!inited(node)));
      node_ptr x = NodeTraits::get_parent(node);
      if(x){
         while(!is_header(x)){
            x = NodeTraits::get_parent(x);
         }
         return x;
      }
      else{
         return node;
      }
   }

   private:
   template<class NodePtrCompare>
   static void insert_equal_check_impl
      (bool upper, const node_ptr & h, const node_ptr & new_node, NodePtrCompare comp, insert_commit_data & commit_data, std::size_t *pdepth = 0)
   {
      std::size_t depth = 0;
      node_ptr y(h);
      node_ptr x(NodeTraits::get_parent(y));
      bool link_left;

      if(upper){
         while(x){
            ++depth;
            y = x;
            x = comp(new_node, x) ? 
                  NodeTraits::get_left(x) : NodeTraits::get_right(x);
         }
         link_left = (y == h) || comp(new_node, y);
      }
      else{
         while(x){
            ++depth;
            y = x;
            x = !comp(x, new_node) ? 
                  NodeTraits::get_left(x) : NodeTraits::get_right(x);
         }
         link_left = (y == h) || !comp(y, new_node);
      }

      commit_data.link_left = link_left;
      commit_data.node = y;
      if(pdepth)  *pdepth = depth;
   }

   static void erase_impl(const node_ptr & header, const node_ptr & z, data_for_rebalance &info)
   {
      node_ptr y(z);
      node_ptr x;
      node_ptr x_parent = node_ptr();
      node_ptr z_left(NodeTraits::get_left(z));
      node_ptr z_right(NodeTraits::get_right(z));
      if(!z_left){
         x = z_right;    // x might be null.
      }
      else if(!z_right){ // z has exactly one non-null child. y == z.
         x = z_left;       // x is not null.
      }
      else{
         // find z's successor
         y = tree_algorithms::minimum (z_right);
         x = NodeTraits::get_right(y);     // x might be null.
      }

      if(y != z){
         // relink y in place of z.  y is z's successor
         NodeTraits::set_parent(NodeTraits::get_left(z), y);
         NodeTraits::set_left(y, NodeTraits::get_left(z));
         if(y != NodeTraits::get_right(z)){
            x_parent = NodeTraits::get_parent(y);
            if(x)
               NodeTraits::set_parent(x, x_parent);
            NodeTraits::set_left(x_parent, x);   // y must be a child of left_
            NodeTraits::set_right(y, NodeTraits::get_right(z));
            NodeTraits::set_parent(NodeTraits::get_right(z), y);
         }
         else
            x_parent = y;
         tree_algorithms::replace_own (z, y, header);
         NodeTraits::set_parent(y, NodeTraits::get_parent(z));
      }
      else {   // y == z --> z has only one child, or none
         x_parent = NodeTraits::get_parent(z);
         if(x)
            NodeTraits::set_parent(x, x_parent);
         tree_algorithms::replace_own (z, x, header);
         if(NodeTraits::get_left(header) == z){
            NodeTraits::set_left(header, !NodeTraits::get_right(z) ?        // z->get_left() must be null also
               NodeTraits::get_parent(z) :  // makes leftmost == header if z == root
               tree_algorithms::minimum (x));
         }
         if(NodeTraits::get_right(header) == z){
            NodeTraits::set_right(header, !NodeTraits::get_left(z) ?        // z->get_right() must be null also
                              NodeTraits::get_parent(z) :  // makes rightmost == header if z == root
                              tree_algorithms::maximum(x));
         }
      }

      info.x = x;
      info.x_parent = x_parent;
      info.y = y;
   }
};

}  //namespace detail {
}  //namespace intrusive 
}  //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREE_ALGORITHMS_HPP
