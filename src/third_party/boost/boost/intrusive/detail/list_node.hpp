/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2009
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_LIST_NODE_HPP
#define BOOST_INTRUSIVE_LIST_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost {
namespace intrusive {

// list_node_traits can be used with circular_list_algorithms and supplies
// a list_node holding the pointers needed for a double-linked list
// it is used by list_derived_node and list_member_node

template<class VoidPointer>
struct list_node
{
   typedef typename pointer_traits
      <VoidPointer>:: template rebind_pointer<list_node>::type    node_ptr;
   node_ptr next_;
   node_ptr prev_;
};

template<class VoidPointer>
struct list_node_traits
{
   typedef list_node<VoidPointer> node;
   typedef typename pointer_traits
      <VoidPointer>:: template rebind_pointer<node>::type         node_ptr;
   typedef typename pointer_traits
      <VoidPointer>:: template rebind_pointer<const node>::type   const_node_ptr;

   static const node_ptr &get_previous(const const_node_ptr & n)
   {  return n->prev_;  }

   static void set_previous(const node_ptr & n, const node_ptr & prev)
   {  n->prev_ = prev;  }

   static const node_ptr &get_next(const const_node_ptr & n)
   {  return n->next_;  }

   static void set_next(const node_ptr & n, const node_ptr & next)
   {  n->next_ = next;  }
};

// list_iterator provides some basic functions for a 
// node oriented bidirectional iterator:
template<class Container, bool IsConst>
class list_iterator
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
   typedef typename real_value_traits::node_traits node_traits;
   typedef typename node_traits::node              node;
   typedef typename node_traits::node_ptr          node_ptr;
   typedef typename pointer_traits<node_ptr>::
      template rebind_pointer<void>::type          void_pointer;
   static const bool store_container_ptr = 
      detail::store_cont_ptr_on_it<Container>::value;

   public:
   typedef typename Container::value_type    value_type;
   typedef typename detail::if_c<IsConst,typename Container::const_pointer,typename Container::pointer>::type pointer;
   typedef typename detail::if_c<IsConst,typename Container::const_reference,typename Container::reference>::type reference;

   list_iterator()
      : members_ (node_ptr(), 0)
   {}

   explicit list_iterator(const node_ptr & node, const Container *cont_ptr)
      : members_ (node, cont_ptr)
   {}

   list_iterator(list_iterator<Container, false> const& other)
      :  members_(other.pointed_node(), other.get_container())
   {}

   const node_ptr &pointed_node() const
   { return members_.nodeptr_; }

   list_iterator &operator=(const node_ptr &node)
   {  members_.nodeptr_ = node;  return static_cast<list_iterator&>(*this);  }

   public:
   list_iterator& operator++() 
   {
      node_ptr p = node_traits::get_next(members_.nodeptr_);
      members_.nodeptr_ = p;
      //members_.nodeptr_ = node_traits::get_next(members_.nodeptr_); 
      return static_cast<list_iterator&> (*this); 
   }
   
   list_iterator operator++(int)
   {
      list_iterator result (*this);
      members_.nodeptr_ = node_traits::get_next(members_.nodeptr_);
      return result;
   }

   list_iterator& operator--() 
   { 
      members_.nodeptr_ = node_traits::get_previous(members_.nodeptr_); 
      return static_cast<list_iterator&> (*this); 
   }
   
   list_iterator operator--(int)
   {
      list_iterator result (*this);
      members_.nodeptr_ = node_traits::get_previous(members_.nodeptr_);
      return result;
   }

   friend bool operator== (const list_iterator& l, const list_iterator& r)
   {  return l.pointed_node() == r.pointed_node();   }

   friend bool operator!= (const list_iterator& l, const list_iterator& r)
   {  return !(l == r); }

   reference operator*() const
   {  return *operator->();   }

   pointer operator->() const
   { return this->get_real_value_traits()->to_value_ptr(members_.nodeptr_); }

   const Container *get_container() const
   {
      if(store_container_ptr){
         const Container* c = static_cast<const Container*>(members_.get_ptr());
         BOOST_INTRUSIVE_INVARIANT_ASSERT(c != 0);
         return c;
      }
      else{
         return 0;
      }
   }

   const real_value_traits *get_real_value_traits() const
   {
      if(store_container_ptr)
         return &this->get_container()->get_real_value_traits();
      else
         return 0;
   }

   list_iterator<Container, false> unconst() const
   {  return list_iterator<Container, false>(this->pointed_node(), this->get_container());   }

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

#endif //BOOST_INTRUSIVE_LIST_NODE_HPP
