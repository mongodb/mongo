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

#ifndef BOOST_INTRUSIVE_TREE_NODE_HPP
#define BOOST_INTRUSIVE_TREE_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

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

   static node_ptr get_parent(const const_node_ptr & n)
   {  return n->parent_;  }

   static node_ptr get_parent(const node_ptr & n)
   {  return n->parent_;  }

   static void set_parent(const node_ptr & n, const node_ptr & p)
   {  n->parent_ = p;  }

   static node_ptr get_left(const const_node_ptr & n)
   {  return n->left_;  }

   static node_ptr get_left(const node_ptr & n)
   {  return n->left_;  }

   static void set_left(const node_ptr & n, const node_ptr & l)
   {  n->left_ = l;  }

   static node_ptr get_right(const const_node_ptr & n)
   {  return n->right_;  }

   static node_ptr get_right(const node_ptr & n)
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
template<class ValueTraits, bool IsConst>
class tree_iterator
{
   protected:
   typedef iiterator< ValueTraits, IsConst
                    , std::bidirectional_iterator_tag>   types_t;

   typedef ValueTraits                                   value_traits;
   typedef typename types_t::node_traits                 node_traits;

   typedef typename types_t::node                        node;
   typedef typename types_t::node_ptr                    node_ptr;
   typedef typename types_t::const_value_traits_ptr      const_value_traits_ptr;
   static const bool stateful_value_traits = types_t::stateful_value_traits;
   typedef bstree_algorithms<node_traits>                node_algorithms;

   public:
   typedef typename types_t::iterator_traits::difference_type    difference_type;
   typedef typename types_t::iterator_traits::value_type         value_type;
   typedef typename types_t::iterator_traits::pointer            pointer;
   typedef typename types_t::iterator_traits::reference          reference;
   typedef typename types_t::iterator_traits::iterator_category  iterator_category;

   tree_iterator()
   {}

   explicit tree_iterator(const node_ptr & nodeptr, const const_value_traits_ptr &traits_ptr)
      : members_(nodeptr, traits_ptr)
   {}

   tree_iterator(tree_iterator<value_traits, false> const& other)
      :  members_(other.pointed_node(), other.get_value_traits())
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
   { return this->operator_arrow(detail::bool_<stateful_value_traits>()); }

   const_value_traits_ptr get_value_traits() const
   {  return members_.get_ptr();  }

   tree_iterator end_iterator_from_it() const
   {
      return tree_iterator(node_algorithms::get_header(this->pointed_node()), this->get_value_traits());
   }

   tree_iterator<value_traits, false> unconst() const
   {  return tree_iterator<value_traits, false>(this->pointed_node(), this->get_value_traits());   }

   private:
   pointer operator_arrow(detail::false_) const
   { return ValueTraits::to_value_ptr(members_.nodeptr_); }

   pointer operator_arrow(detail::true_) const
   { return this->get_value_traits()->to_value_ptr(members_.nodeptr_); }

   iiterator_members<node_ptr, const_value_traits_ptr, stateful_value_traits> members_;
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREE_NODE_HPP
