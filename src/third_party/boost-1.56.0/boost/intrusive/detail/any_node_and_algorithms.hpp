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

#ifndef BOOST_INTRUSIVE_ANY_NODE_HPP
#define BOOST_INTRUSIVE_ANY_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <cstddef>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {

template<class VoidPointer>
struct any_node
{
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<any_node>::type   node_ptr;
   node_ptr    node_ptr_1;
   node_ptr    node_ptr_2;
   node_ptr    node_ptr_3;
   std::size_t size_t_1;
};

template<class VoidPointer>
struct any_list_node_traits
{
   typedef any_node<VoidPointer> node;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type const_node_ptr;

   static const node_ptr &get_next(const const_node_ptr & n)
   {  return n->node_ptr_1;  }

   static void set_next(const node_ptr & n, const node_ptr & next)
   {  n->node_ptr_1 = next;  }

   static const node_ptr &get_previous(const const_node_ptr & n)
   {  return n->node_ptr_2;  }

   static void set_previous(const node_ptr & n, const node_ptr & prev)
   {  n->node_ptr_2 = prev;  }
};


template<class VoidPointer>
struct any_slist_node_traits
{
   typedef any_node<VoidPointer> node;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;

   static const node_ptr &get_next(const const_node_ptr & n)
   {  return n->node_ptr_1;  }

   static void set_next(const node_ptr & n, const node_ptr & next)
   {  n->node_ptr_1 = next;  }
};


template<class VoidPointer>
struct any_unordered_node_traits
   :  public any_slist_node_traits<VoidPointer>
{
   typedef any_slist_node_traits<VoidPointer>                  reduced_slist_node_traits;
   typedef typename reduced_slist_node_traits::node            node;
   typedef typename reduced_slist_node_traits::node_ptr        node_ptr;
   typedef typename reduced_slist_node_traits::const_node_ptr  const_node_ptr;

   static const bool store_hash        = true;
   static const bool optimize_multikey = true;

   static const node_ptr &get_next(const const_node_ptr & n)
   {  return n->node_ptr_1;   }

   static void set_next(const node_ptr & n, const node_ptr & next)
   {  n->node_ptr_1 = next;  }

   static node_ptr get_prev_in_group(const const_node_ptr & n)
   {  return n->node_ptr_2;  }

   static void set_prev_in_group(const node_ptr & n, const node_ptr & prev)
   {  n->node_ptr_2 = prev;  }

   static std::size_t get_hash(const const_node_ptr & n)
   {  return n->size_t_1;  }

   static void set_hash(const node_ptr & n, std::size_t h)
   {  n->size_t_1 = h;  }
};


template<class VoidPointer>
struct any_rbtree_node_traits
{
   typedef any_node<VoidPointer> node;

   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;

   typedef std::size_t color;

   static const node_ptr &get_parent(const const_node_ptr & n)
   {  return n->node_ptr_1;  }

   static void set_parent(const node_ptr & n, const node_ptr & p)
   {  n->node_ptr_1 = p;  }

   static const node_ptr &get_left(const const_node_ptr & n)
   {  return n->node_ptr_2;  }

   static void set_left(const node_ptr & n, const node_ptr & l)
   {  n->node_ptr_2 = l;  }

   static const node_ptr &get_right(const const_node_ptr & n)
   {  return n->node_ptr_3;  }

   static void set_right(const node_ptr & n, const node_ptr & r)
   {  n->node_ptr_3 = r;  }

   static color get_color(const const_node_ptr & n)
   {  return n->size_t_1;  }

   static void set_color(const node_ptr & n, color c)
   {  n->size_t_1 = c;  }

   static color black()
   {  return 0u;  }

   static color red()
   {  return 1u;  }
};


template<class VoidPointer>
struct any_avltree_node_traits
{
   typedef any_node<VoidPointer> node;

   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;
   typedef std::size_t balance;

   static const node_ptr &get_parent(const const_node_ptr & n)
   {  return n->node_ptr_1;  }

   static void set_parent(const node_ptr & n, const node_ptr & p)
   {  n->node_ptr_1 = p;  }

   static const node_ptr &get_left(const const_node_ptr & n)
   {  return n->node_ptr_2;  }

   static void set_left(const node_ptr & n, const node_ptr & l)
   {  n->node_ptr_2 = l;  }

   static const node_ptr &get_right(const const_node_ptr & n)
   {  return n->node_ptr_3;  }

   static void set_right(const node_ptr & n, const node_ptr & r)
   {  n->node_ptr_3 = r;  }

   static balance get_balance(const const_node_ptr & n)
   {  return n->size_t_1;  }

   static void set_balance(const node_ptr & n, balance b)
   {  n->size_t_1 = b;  }

   static balance negative()
   {  return 0u;  }

   static balance zero()
   {  return 1u;  }

   static balance positive()
   {  return 2u;  }
};


template<class VoidPointer>
struct any_tree_node_traits
{
   typedef any_node<VoidPointer> node;

   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;

   static const node_ptr &get_parent(const const_node_ptr & n)
   {  return n->node_ptr_1;  }

   static void set_parent(const node_ptr & n, const node_ptr & p)
   {  n->node_ptr_1 = p;  }

   static const node_ptr &get_left(const const_node_ptr & n)
   {  return n->node_ptr_2;  }

   static void set_left(const node_ptr & n, const node_ptr & l)
   {  n->node_ptr_2 = l;  }

   static const node_ptr &get_right(const const_node_ptr & n)
   {  return n->node_ptr_3;  }

   static void set_right(const node_ptr & n, const node_ptr & r)
   {  n->node_ptr_3 = r;  }
};

template<class VoidPointer>
class any_node_traits
{
   public:
   typedef any_node<VoidPointer>          node;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;
};

template<class VoidPointer>
class any_algorithms
{
   template <class T>
   static void function_not_available_for_any_hooks(typename detail::enable_if<detail::is_same<T, bool> >::type)
   {}

   public:
   typedef any_node<VoidPointer>             node;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer<const node>::type   const_node_ptr;
   typedef any_node_traits<VoidPointer>      node_traits;

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
   {  node->node_ptr_1 = 0;   };

   //! <b>Effects</b>: Returns true if node is in the same state as if called init(node)
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   static bool inited(const const_node_ptr & node)
   {  return !node->node_ptr_1;  };

   static bool unique(const const_node_ptr & node)
   {  return 0 == node->node_ptr_1; }

   static void unlink(const node_ptr &)
   {
      //Auto-unlink hooks and unlink() are not available for any hooks
      any_algorithms<VoidPointer>::template function_not_available_for_any_hooks<node_ptr>();
   }

   static void swap_nodes(const node_ptr & l, const node_ptr & r)
   {
      //Any nodes have no swap_nodes capability because they don't know
      //what algorithm they must use to unlink the node from the container
      any_algorithms<VoidPointer>::template function_not_available_for_any_hooks<node_ptr>();
   }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_ANY_NODE_HPP
