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

#ifndef BOOST_INTRUSIVE_TREE_NODE_HPP
#define BOOST_INTRUSIVE_TREE_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {

template<class VoidPointer>
struct tree_node
{
   typedef typename pointer_traits
      <VoidPointer>::template rebind_pointer
         <tree_node<VoidPointer> >::type   node_ptr;

   node_ptr parent_, left_, right_;
};

template<class VoidPointer>
struct tree_node_traits
{
   typedef tree_node<VoidPointer> node;

   typedef typename pointer_traits<VoidPointer>::template
      rebind_pointer<node>::type              node_ptr;
   typedef typename pointer_traits<VoidPointer>::template
      rebind_pointer<const node>::type        const_node_ptr;

   static const node_ptr & get_parent(const const_node_ptr & n)
   {  return n->parent_;  }

   static void set_parent(const node_ptr & n, const node_ptr & p)
   {  n->parent_ = p;  }

   static const node_ptr & get_left(const const_node_ptr & n)
   {  return n->left_;  }

   static void set_left(const node_ptr & n, const node_ptr & l)
   {  n->left_ = l;  }

   static const node_ptr & get_right(const const_node_ptr & n)
   {  return n->right_;  }

   static void set_right(const node_ptr & n, const node_ptr & r)
   {  n->right_ = r;  }
};

/////////////////////////////////////////////////////////////////////////////
//                                                                         //
//                   Implementation of the tree iterator                   //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

// tree_iterator provides some basic functions for a 
// node oriented bidirectional iterator:
template<class Container, bool IsConst>
class tree_iterator
   :  public std::iterator
         < std::bidirectional_iterator_tag
         , typename Container::value_type
         , typename Container::difference_type
         , typename detail::if_c<IsConst,typename Container::const_pointer,typename Container::pointer>::type
         , typename detail::if_c<IsConst,typename Container::const_reference,typename Container::reference>::type
         >
{
   protected:
   typedef typename Container::real_value_traits   real_value_traits;
   typedef typename Container::node_algorithms     node_algorithms;
   typedef typename real_value_traits::node_traits node_traits;
   typedef typename node_traits::node              node;
   typedef typename node_traits::node_ptr          node_ptr;
   typedef typename pointer_traits<node_ptr>::template
      rebind_pointer<void>::type                   void_pointer;
   static const bool store_container_ptr = 
      detail::store_cont_ptr_on_it<Container>::value;

   public:
   typedef typename Container::value_type    value_type;
   typedef typename detail::if_c<IsConst,typename Container::const_pointer,typename Container::pointer>::type pointer;
   typedef typename detail::if_c<IsConst,typename Container::const_reference,typename Container::reference>::type reference;


   tree_iterator()
      : members_ (node_ptr(), (const void *)0)
   {}

   explicit tree_iterator(const node_ptr & nodeptr, const Container *cont_ptr)
      : members_ (nodeptr, cont_ptr)
   {}

   tree_iterator(tree_iterator<Container, false> const& other)
      :  members_(other.pointed_node(), other.get_container())
   {}

   const node_ptr &pointed_node() const
   { return members_.nodeptr_; }

   tree_iterator &operator=(const node_ptr &nodeptr)
   {  members_.nodeptr_ = nodeptr;  return static_cast<tree_iterator&>(*this);  }

   public:
   tree_iterator& operator++() 
   { 
      members_.nodeptr_ = node_algorithms::next_node(members_.nodeptr_); 
      return static_cast<tree_iterator&> (*this); 
   }
   
   tree_iterator operator++(int)
   {
      tree_iterator result (*this);
      members_.nodeptr_ = node_algorithms::next_node(members_.nodeptr_);
      return result;
   }

   tree_iterator& operator--() 
   { 
      members_.nodeptr_ = node_algorithms::prev_node(members_.nodeptr_); 
      return static_cast<tree_iterator&> (*this); 
   }
   
   tree_iterator operator--(int)
   {
      tree_iterator result (*this);
      members_.nodeptr_ = node_algorithms::prev_node(members_.nodeptr_);
      return result;
   }

   friend bool operator== (const tree_iterator& l, const tree_iterator& r)
   { return l.pointed_node() == r.pointed_node(); }

   friend bool operator!= (const tree_iterator& l, const tree_iterator& r)
   {  return !(l == r);   }

   reference operator*() const
   {  return *operator->();   }

   pointer operator->() const
   { return this->get_real_value_traits()->to_value_ptr(members_.nodeptr_); }

   const Container *get_container() const
   {  return static_cast<const Container*>(members_.get_ptr());   }

   const real_value_traits *get_real_value_traits() const
   {  return &this->get_container()->get_real_value_traits();  }

   tree_iterator end_iterator_from_it() const
   {
      return tree_iterator(node_algorithms::get_header(this->pointed_node()), this->get_container());
   }

   tree_iterator<Container, false> unconst() const
   {  return tree_iterator<Container, false>(this->pointed_node(), this->get_container());   }

   private:
   struct members
      :  public detail::select_constptr
         <void_pointer, store_container_ptr>::type
   {
      typedef typename detail::select_constptr
         <void_pointer, store_container_ptr>::type Base;

      members(const node_ptr &n_ptr, const void *cont)
         :  Base(cont), nodeptr_(n_ptr)
      {}

      node_ptr nodeptr_;
   } members_;
};

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREE_NODE_HPP
