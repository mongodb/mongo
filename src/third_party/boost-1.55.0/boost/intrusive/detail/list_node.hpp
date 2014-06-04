/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2013
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

   static node_ptr get_previous(const const_node_ptr & n)
   {  return n->prev_;  }

   static node_ptr get_previous(const node_ptr & n)
   {  return n->prev_;  }

   static void set_previous(const node_ptr & n, const node_ptr & prev)
   {  n->prev_ = prev;  }

   static node_ptr get_next(const const_node_ptr & n)
   {  return n->next_;  }

   static node_ptr get_next(const node_ptr & n)
   {  return n->next_;  }

   static void set_next(const node_ptr & n, const node_ptr & next)
   {  n->next_ = next;  }
};

// list_iterator provides some basic functions for a
// node oriented bidirectional iterator:
template<class RealValueTraits, bool IsConst>
class list_iterator
   :  public iiterator<RealValueTraits, IsConst, std::bidirectional_iterator_tag>::iterator_base
{
   protected:
   typedef iiterator
      <RealValueTraits, IsConst, std::bidirectional_iterator_tag> types_t;

   static const bool stateful_value_traits =                types_t::stateful_value_traits;

   typedef RealValueTraits                                  real_value_traits;
   typedef typename types_t::node_traits                    node_traits;

   typedef typename types_t::node                           node;
   typedef typename types_t::node_ptr                       node_ptr;
   typedef typename types_t::void_pointer                   void_pointer;

   public:
   typedef typename types_t::value_type      value_type;
   typedef typename types_t::pointer         pointer;
   typedef typename types_t::reference       reference;

   typedef typename pointer_traits
      <void_pointer>::template rebind_pointer
         <const real_value_traits>::type   const_real_value_traits_ptr;

   list_iterator()
   {}

   explicit list_iterator(const node_ptr & nodeptr, const const_real_value_traits_ptr &traits_ptr)
      : members_(nodeptr, traits_ptr)
   {}

   list_iterator(list_iterator<RealValueTraits, false> const& other)
      :  members_(other.pointed_node(), other.get_real_value_traits())
   {}

   const node_ptr &pointed_node() const
   { return members_.nodeptr_; }

   list_iterator &operator=(const node_ptr &node)
   {  members_.nodeptr_ = node;  return static_cast<list_iterator&>(*this);  }

   const_real_value_traits_ptr get_real_value_traits() const
   {  return pointer_traits<const_real_value_traits_ptr>::static_cast_from(members_.get_ptr()); }

   public:
   list_iterator& operator++()
   {
      node_ptr p = node_traits::get_next(members_.nodeptr_);
      members_.nodeptr_ = p;
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

   list_iterator<RealValueTraits, false> unconst() const
   {  return list_iterator<RealValueTraits, false>(this->pointed_node(), this->get_real_value_traits());   }

   private:
   iiterator_members<node_ptr, stateful_value_traits> members_;
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_LIST_NODE_HPP
