//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//

#ifndef BOOST_CONTAINER_LIST_HPP
#define BOOST_CONTAINER_LIST_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/throw_exception.hpp>
#include <boost/move/utility.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/detail/move_helpers.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/algorithms.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/assert.hpp>
#include <boost/container/detail/node_alloc_holder.hpp>

#if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
#else
//Preprocessor library to emulate perfect forwarding
#include <boost/container/detail/preprocessor.hpp>
#endif

#include <iterator>
#include <utility>
#include <memory>
#include <functional>
#include <algorithm>

namespace boost {
namespace container {

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
namespace container_detail {

template<class VoidPointer>
struct list_hook
{
   typedef typename container_detail::bi::make_list_base_hook
      <container_detail::bi::void_pointer<VoidPointer>, container_detail::bi::link_mode<container_detail::bi::normal_link> >::type type;
};

template <class T, class VoidPointer>
struct list_node
   :  public list_hook<VoidPointer>::type
{
   private:
   list_node();

   public:
   typedef T value_type;
   typedef typename list_hook<VoidPointer>::type hook_type;

   T m_data;

   T &get_data()
   {  return this->m_data;   }

   const T &get_data() const
   {  return this->m_data;   }
};

template<class Allocator>
struct intrusive_list_type
{
   typedef boost::container::allocator_traits<Allocator>   allocator_traits_type;
   typedef typename allocator_traits_type::value_type value_type;
   typedef typename boost::intrusive::pointer_traits
      <typename allocator_traits_type::pointer>::template
         rebind_pointer<void>::type
            void_pointer;
   typedef typename container_detail::list_node
         <value_type, void_pointer>             node_type;
   typedef typename container_detail::bi::make_list
      < node_type
      , container_detail::bi::base_hook<typename list_hook<void_pointer>::type>
      , container_detail::bi::constant_time_size<true>
      , container_detail::bi::size_type
         <typename allocator_traits_type::size_type>
      >::type                                   container_type;
   typedef container_type                       type ;
};

}  //namespace container_detail {
#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//! A list is a doubly linked list. That is, it is a Sequence that supports both
//! forward and backward traversal, and (amortized) constant time insertion and
//! removal of elements at the beginning or the end, or in the middle. Lists have
//! the important property that insertion and splicing do not invalidate iterators
//! to list elements, and that even removal invalidates only the iterators that point
//! to the elements that are removed. The ordering of iterators may be changed
//! (that is, list<T>::iterator might have a different predecessor or successor
//! after a list operation than it did before), but the iterators themselves will
//! not be invalidated or made to point to different elements unless that invalidation
//! or mutation is explicit.
//!
//! \tparam T The type of object that is stored in the list
//! \tparam Allocator The allocator used for all internal memory management
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Allocator = std::allocator<T> >
#else
template <class T, class Allocator>
#endif
class list
   : protected container_detail::node_alloc_holder
      <Allocator, typename container_detail::intrusive_list_type<Allocator>::type>
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   typedef typename
      container_detail::intrusive_list_type<Allocator>::type Icont;
   typedef container_detail::node_alloc_holder<Allocator, Icont>  AllocHolder;
   typedef typename AllocHolder::NodePtr                          NodePtr;
   typedef typename AllocHolder::NodeAlloc                        NodeAlloc;
   typedef typename AllocHolder::ValAlloc                         ValAlloc;
   typedef typename AllocHolder::Node                             Node;
   typedef container_detail::allocator_destroyer<NodeAlloc>       Destroyer;
   typedef typename AllocHolder::allocator_v1                     allocator_v1;
   typedef typename AllocHolder::allocator_v2                     allocator_v2;
   typedef typename AllocHolder::alloc_version                    alloc_version;
   typedef boost::container::allocator_traits<Allocator>          allocator_traits_type;

   class equal_to_value
   {
      typedef typename AllocHolder::value_type value_type;
      const value_type &t_;

      public:
      equal_to_value(const value_type &t)
         :  t_(t)
      {}

      bool operator()(const value_type &t)const
      {  return t_ == t;   }
   };

   template<class Pred>
   struct ValueCompareToNodeCompare
      :  Pred
   {
      ValueCompareToNodeCompare(Pred pred)
         :  Pred(pred)
      {}

      bool operator()(const Node &a, const Node &b) const
      {  return static_cast<const Pred&>(*this)(a.m_data, b.m_data);  }

      bool operator()(const Node &a) const
      {  return static_cast<const Pred&>(*this)(a.m_data);  }
   };

   BOOST_COPYABLE_AND_MOVABLE(list)

   typedef container_detail::iterator<typename Icont::iterator, false>  iterator_impl;
   typedef container_detail::iterator<typename Icont::iterator, true>   const_iterator_impl;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   //////////////////////////////////////////////
   //
   //                    types
   //
   //////////////////////////////////////////////

   typedef T                                                                           value_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer           pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_pointer     const_pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::reference         reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_reference   const_reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::size_type         size_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::difference_type   difference_type;
   typedef Allocator                                                                   allocator_type;
   typedef BOOST_CONTAINER_IMPDEF(NodeAlloc)                                           stored_allocator_type;
   typedef BOOST_CONTAINER_IMPDEF(iterator_impl)                                       iterator;
   typedef BOOST_CONTAINER_IMPDEF(const_iterator_impl)                                 const_iterator;
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<iterator>)                     reverse_iterator;
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<const_iterator>)               const_reverse_iterator;

   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Default constructs a list.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   list()
      : AllocHolder()
   {}

   //! <b>Effects</b>: Constructs a list taking the allocator as parameter.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   explicit list(const allocator_type &a) BOOST_CONTAINER_NOEXCEPT
      : AllocHolder(a)
   {}

   //! <b>Effects</b>: Constructs a list that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor
   //!   throws or T's default or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   explicit list(size_type n)
      : AllocHolder(Allocator())
   {  this->resize(n);  }

   //! <b>Effects</b>: Constructs a list that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor
   //!   throws or T's default or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   list(size_type n, const T& value, const Allocator& a = Allocator())
      : AllocHolder(a)
   {  this->insert(this->cbegin(), n, value);  }

   //! <b>Effects</b>: Copy constructs a list.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   list(const list& x)
      : AllocHolder(x)
   {  this->insert(this->cbegin(), x.begin(), x.end());   }

   //! <b>Effects</b>: Move constructor. Moves mx's resources to *this.
   //!
   //! <b>Throws</b>: If allocator_type's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   list(BOOST_RV_REF(list) x)
      : AllocHolder(boost::move(static_cast<AllocHolder&>(x)))
   {}

   //! <b>Effects</b>: Copy constructs a list using the specified allocator.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   list(const list& x, const allocator_type &a)
      : AllocHolder(a)
   {  this->insert(this->cbegin(), x.begin(), x.end());   }

   //! <b>Effects</b>: Move constructor sing the specified allocator.
   //!                 Moves mx's resources to *this.
   //!
   //! <b>Throws</b>: If allocation or value_type's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant if a == x.get_allocator(), linear otherwise.
   list(BOOST_RV_REF(list) x, const allocator_type &a)
      : AllocHolder(a)
   {
      if(this->node_alloc() == x.node_alloc()){
         this->icont().swap(x.icont());
      }
      else{
         this->insert(this->cbegin(), x.begin(), x.end());
      }
   }

   //! <b>Effects</b>: Constructs a list that will use a copy of allocator a
   //!   and inserts a copy of the range [first, last) in the list.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor
   //!   throws or T's constructor taking a dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InpIt>
   list(InpIt first, InpIt last, const Allocator &a = Allocator())
      : AllocHolder(a)
   {  this->insert(this->cbegin(), first, last);  }

   //! <b>Effects</b>: Destroys the list. All stored values are destroyed
   //!   and used memory is deallocated.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements.
   ~list() BOOST_CONTAINER_NOEXCEPT
   {} //AllocHolder clears the list

   //! <b>Effects</b>: Makes *this contain the same elements as x.
   //!
   //! <b>Postcondition</b>: this->size() == x.size(). *this contains a copy
   //! of each of x's elements.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in x.
   list& operator=(BOOST_COPY_ASSIGN_REF(list) x)
   {
      if (&x != this){
         NodeAlloc &this_alloc     = this->node_alloc();
         const NodeAlloc &x_alloc  = x.node_alloc();
         container_detail::bool_<allocator_traits_type::
            propagate_on_container_copy_assignment::value> flag;
         if(flag && this_alloc != x_alloc){
            this->clear();
         }
         this->AllocHolder::copy_assign_alloc(x);
         this->assign(x.begin(), x.end());
      }
      return *this;
   }

   //! <b>Effects</b>: Move assignment. All x's values are transferred to *this.
   //!
   //! <b>Postcondition</b>: x.empty(). *this contains a the elements x had
   //!   before the function.
   //!
   //! <b>Throws</b>: If allocator_traits_type::propagate_on_container_move_assignment
   //!   is false and (allocation throws or value_type's move constructor throws)
   //!
   //! <b>Complexity</b>: Constant if allocator_traits_type::
   //!   propagate_on_container_move_assignment is true or
   //!   this->get>allocator() == x.get_allocator(). Linear otherwise.
   list& operator=(BOOST_RV_REF(list) x)
      BOOST_CONTAINER_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value)
   {
      BOOST_ASSERT(this != &x);
      NodeAlloc &this_alloc = this->node_alloc();
      NodeAlloc &x_alloc    = x.node_alloc();
      const bool propagate_alloc = allocator_traits_type::
            propagate_on_container_move_assignment::value;
      const bool allocators_equal = this_alloc == x_alloc; (void)allocators_equal;
      //Resources can be transferred if both allocators are
      //going to be equal after this function (either propagated or already equal)
      if(propagate_alloc || allocators_equal){
         //Destroy
         this->clear();
         //Move allocator if needed
         this->AllocHolder::move_assign_alloc(x);
         //Obtain resources
         this->icont() = boost::move(x.icont());
      }
      //Else do a one by one move
      else{
         this->assign( boost::make_move_iterator(x.begin())
                     , boost::make_move_iterator(x.end()));
      }
      return *this;
   }

   //! <b>Effects</b>: Assigns the n copies of val to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   void assign(size_type n, const T& val)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      return this->assign(cvalue_iterator(val, n), cvalue_iterator());
   }

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's constructor from dereferencing InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template <class InpIt>
   void assign(InpIt first, InpIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InpIt, size_type>::value
         >::type * = 0
      #endif
      )
   {
      iterator first1      = this->begin();
      const iterator last1 = this->end();
      for ( ; first1 != last1 && first != last; ++first1, ++first)
         *first1 = *first;
      if (first == last)
         this->erase(first1, last1);
      else{
         this->insert(last1, first, last);
      }
   }

   //! <b>Effects</b>: Returns a copy of the internal allocator.
   //!
   //! <b>Throws</b>: If allocator's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const BOOST_CONTAINER_NOEXCEPT
   {  return allocator_type(this->node_alloc()); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT
   {  return this->node_alloc(); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT
   {  return this->node_alloc(); }

   //////////////////////////////////////////////
   //
   //                iterators
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns an iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator begin() BOOST_CONTAINER_NOEXCEPT
   { return iterator(this->icont().begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const BOOST_CONTAINER_NOEXCEPT
   {  return this->cbegin();   }

   //! <b>Effects</b>: Returns an iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator end() BOOST_CONTAINER_NOEXCEPT
   {  return iterator(this->icont().end());  }

   //! <b>Effects</b>: Returns a const_iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT
   {  return this->cend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT
   {  return reverse_iterator(end());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT
   {  return this->crbegin();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT
   {  return reverse_iterator(begin());   }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT
   {  return this->crend();   }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const BOOST_CONTAINER_NOEXCEPT
   {  return const_iterator(this->non_const_icont().begin());   }

   //! <b>Effects</b>: Returns a const_iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT
   {  return const_iterator(this->non_const_icont().end());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT
   {  return const_reverse_iterator(this->cend());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT
   {  return const_reverse_iterator(this->cbegin());   }

   //////////////////////////////////////////////
   //
   //                capacity
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns true if the list contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   bool empty() const BOOST_CONTAINER_NOEXCEPT
   {  return !this->size();  }

   //! <b>Effects</b>: Returns the number of the elements contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type size() const BOOST_CONTAINER_NOEXCEPT
   {   return this->icont().size();   }

   //! <b>Effects</b>: Returns the largest possible size of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type max_size() const BOOST_CONTAINER_NOEXCEPT
   {  return AllocHolder::max_size();  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are value initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size)
   {
      if(!priv_try_shrink(new_size)){
         typedef value_init_construct_iterator<value_type, difference_type> value_init_iterator;
         this->insert(this->cend(), value_init_iterator(new_size - this->size()), value_init_iterator());
      }
   }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are copy constructed from x.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size, const T& x)
   {
      if(!priv_try_shrink(new_size)){
         this->insert(this->cend(), new_size - this->size(), x);
      }
   }

   //////////////////////////////////////////////
   //
   //               element access
   //
   //////////////////////////////////////////////

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the first element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference front() BOOST_CONTAINER_NOEXCEPT
   { return *this->begin(); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference front() const BOOST_CONTAINER_NOEXCEPT
   { return *this->begin(); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the first element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference back() BOOST_CONTAINER_NOEXCEPT
   { return *(--this->end()); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference back() const BOOST_CONTAINER_NOEXCEPT
   { return *(--this->end()); }

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the list.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's in-place constructor throws.
   //!
   //! <b>Complexity</b>: Constant
   template <class... Args>
   void emplace_back(Args&&... args)
   {  this->emplace(this->cend(), boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the beginning of the list.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's in-place constructor throws.
   //!
   //! <b>Complexity</b>: Constant
   template <class... Args>
   void emplace_front(Args&&... args)
   {  this->emplace(this->cbegin(), boost::forward<Args>(args)...);  }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... before p.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's in-place constructor throws.
   //!
   //! <b>Complexity</b>: Constant
   template <class... Args>
   iterator emplace(const_iterator p, Args&&... args)
   {
      NodePtr pnode(AllocHolder::create_node(boost::forward<Args>(args)...));
      return iterator(this->icont().insert(p.get(), *pnode));
   }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                              \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)       \
   void emplace_back(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                        \
   {                                                                                            \
      this->emplace(this->cend()                                                                \
                    BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));            \
   }                                                                                            \
                                                                                                \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)       \
   void emplace_front(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                       \
   {                                                                                            \
      this->emplace(this->cbegin()                                                              \
                    BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));            \
   }                                                                                            \
                                                                                                \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)       \
   iterator emplace(const_iterator p                                                            \
                    BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                \
   {                                                                                            \
      NodePtr pnode (AllocHolder::create_node                                                   \
         (BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)));                              \
      return iterator(this->icont().insert(p.get(), *pnode));                                   \
   }                                                                                            \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x at the beginning of the list.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_front(const T &x);

   //! <b>Effects</b>: Constructs a new element in the beginning of the list
   //!   and moves the resources of mx to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_front(T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_front, T, void, priv_push_front)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x at the end of the list.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(const T &x);

   //! <b>Effects</b>: Constructs a new element in the end of the list
   //!   and moves the resources of mx to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_back, T, void, priv_push_back)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of x before position.
   //!
   //! <b>Returns</b>: an iterator to the inserted element.
   //!
   //! <b>Throws</b>: If memory allocation throws or x's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   iterator insert(const_iterator position, const T &x);

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a new element before position with mx's resources.
   //!
   //! <b>Returns</b>: an iterator to the inserted element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   iterator insert(const_iterator position, T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, T, iterator, priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Inserts n copies of x before p.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or p if n is 0.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   iterator insert(const_iterator p, size_type n, const T& x)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      return this->insert(p, cvalue_iterator(x, n), cvalue_iterator());
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before p.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or p if first == last.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to std::distance [first, last).
   template <class InpIt>
   iterator insert(const_iterator p, InpIt first, InpIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InpIt, size_type>::value
            && (container_detail::is_input_iterator<InpIt>::value
                || container_detail::is_same<alloc_version, allocator_v1>::value
               )
         >::type * = 0
      #endif
      )
   {
      const typename Icont::iterator ipos(p.get());
      iterator ret_it(ipos);
      if(first != last){
         ret_it = iterator(this->icont().insert(ipos, *this->create_node_from_it(first)));
         ++first;
      }
      for (; first != last; ++first){
         this->icont().insert(ipos, *this->create_node_from_it(first));
      }
      return ret_it;
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class FwdIt>
   iterator insert(const_iterator p, FwdIt first, FwdIt last
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<FwdIt, size_type>::value
            && !(container_detail::is_input_iterator<FwdIt>::value
                || container_detail::is_same<alloc_version, allocator_v1>::value
               )
         >::type * = 0
      )
   {
      //Optimized allocation and construction
      insertion_functor func(this->icont(), p.get());
      iterator before_p(p.get());
      --before_p;
      this->allocate_many_and_construct(first, std::distance(first, last), func);
      return ++before_p;
   }
   #endif

   //! <b>Effects</b>: Removes the first element from the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void pop_front() BOOST_CONTAINER_NOEXCEPT
   {  this->erase(this->cbegin());      }

   //! <b>Effects</b>: Removes the last element from the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void pop_back() BOOST_CONTAINER_NOEXCEPT
   {  const_iterator tmp = this->cend(); this->erase(--tmp);  }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Erases the element at p p.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   iterator erase(const_iterator p) BOOST_CONTAINER_NOEXCEPT
   {  return iterator(this->icont().erase_and_dispose(p.get(), Destroyer(this->node_alloc()))); }

   //! <b>Requires</b>: first and last must be valid iterator to elements in *this.
   //!
   //! <b>Effects</b>: Erases the elements pointed by [first, last).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the distance between first and last.
   iterator erase(const_iterator first, const_iterator last) BOOST_CONTAINER_NOEXCEPT
   {  return iterator(AllocHolder::erase_range(first.get(), last.get(), alloc_version())); }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(list& x)
   {  AllocHolder::swap(x);   }

   //! <b>Effects</b>: Erases all the elements of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the list.
   void clear() BOOST_CONTAINER_NOEXCEPT
   {  AllocHolder::clear(alloc_version());  }

   //////////////////////////////////////////////
   //
   //              slist operations
   //
   //////////////////////////////////////////////

   //! <b>Requires</b>: p must point to an element contained
   //!   by the list. x != *this. this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers all the elements of list x to this list, before the
   //!   the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of
   //!    this list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, list& x) BOOST_CONTAINER_NOEXCEPT
   {
      BOOST_ASSERT(this != &x);
      BOOST_ASSERT(this->node_alloc() == x.node_alloc());
      this->icont().splice(p.get(), x.icont());
   }

   //! <b>Requires</b>: p must point to an element contained
   //!   by the list. x != *this. this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers all the elements of list x to this list, before the
   //!   the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of
   //!    this list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, BOOST_RV_REF(list) x) BOOST_CONTAINER_NOEXCEPT
   {  this->splice(p, static_cast<list&>(x));  }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. i must point to an element contained in list x.
   //!   this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers the value pointed by i, from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!   If p == i or p == ++i, this function is a null operation.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, list &x, const_iterator i) BOOST_CONTAINER_NOEXCEPT
   {
      //BOOST_ASSERT(this != &x);
      BOOST_ASSERT(this->node_alloc() == x.node_alloc());
      this->icont().splice(p.get(), x.icont(), i.get());
   }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. i must point to an element contained in list x.
   //!   this' allocator and x's allocator shall compare equal.
   //!
   //! <b>Effects</b>: Transfers the value pointed by i, from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!   If p == i or p == ++i, this function is a null operation.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, BOOST_RV_REF(list) x, const_iterator i) BOOST_CONTAINER_NOEXCEPT
   {  this->splice(p, static_cast<list&>(x), i);  }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. first and last must point to elements contained in list x.
   //!   this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers the range pointed by first and last from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, list &x, const_iterator first, const_iterator last) BOOST_CONTAINER_NOEXCEPT
   {
      BOOST_ASSERT(this->node_alloc() == x.node_alloc());
      this->icont().splice(p.get(), x.icont(), first.get(), last.get());
   }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. first and last must point to elements contained in list x.
   //!   this' allocator and x's allocator shall compare equal.
   //!
   //! <b>Effects</b>: Transfers the range pointed by first and last from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(const_iterator p, BOOST_RV_REF(list) x, const_iterator first, const_iterator last) BOOST_CONTAINER_NOEXCEPT
   {  this->splice(p, static_cast<list&>(x), first, last);  }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. first and last must point to elements contained in list x.
   //!   n == std::distance(first, last). this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers the range pointed by first and last from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>:  Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Note</b>: Non-standard extension
   void splice(const_iterator p, list &x, const_iterator first, const_iterator last, size_type n) BOOST_CONTAINER_NOEXCEPT
   {
      BOOST_ASSERT(this->node_alloc() == x.node_alloc());
      this->icont().splice(p.get(), x.icont(), first.get(), last.get(), n);
   }

   //! <b>Requires</b>: p must point to an element contained
   //!   by this list. first and last must point to elements contained in list x.
   //!   n == std::distance(first, last). this' allocator and x's allocator shall compare equal
   //!
   //! <b>Effects</b>: Transfers the range pointed by first and last from list x to this list,
   //!   before the the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Note</b>: Non-standard extension
   void splice(const_iterator p, BOOST_RV_REF(list) x, const_iterator first, const_iterator last, size_type n) BOOST_CONTAINER_NOEXCEPT
   {  this->splice(p, static_cast<list&>(x), first, last, n);  }

   //! <b>Effects</b>: Removes all the elements that compare equal to value.
   //!
   //! <b>Throws</b>: If comparison throws.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() comparisons for equality.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   void remove(const T& value)
   {  this->remove_if(equal_to_value(value));  }

   //! <b>Effects</b>: Removes all the elements for which a specified
   //!   predicate is satisfied.
   //!
   //! <b>Throws</b>: If pred throws.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() calls to the predicate.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template <class Pred>
   void remove_if(Pred pred)
   {
      typedef ValueCompareToNodeCompare<Pred> Predicate;
      this->icont().remove_and_dispose_if(Predicate(pred), Destroyer(this->node_alloc()));
   }

   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that are equal from the list.
   //!
   //! <b>Throws</b>: If comparison throws.
   //!
   //! <b>Complexity</b>: Linear time (size()-1 comparisons equality comparisons).
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   void unique()
   {  this->unique(value_equal());  }

   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that satisfy some binary predicate from the list.
   //!
   //! <b>Throws</b>: If pred throws.
   //!
   //! <b>Complexity</b>: Linear time (size()-1 comparisons calls to pred()).
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template <class BinaryPredicate>
   void unique(BinaryPredicate binary_pred)
   {
      typedef ValueCompareToNodeCompare<BinaryPredicate> Predicate;
      this->icont().unique_and_dispose(Predicate(binary_pred), Destroyer(this->node_alloc()));
   }

   //! <b>Requires</b>: The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this according to std::less<value_type>. The merge is stable;
   //!   that is, if an element from *this is equivalent to one from x, then the element
   //!   from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If comparison throws.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   void merge(list &x)
   {  this->merge(x, value_less());  }

   //! <b>Requires</b>: The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this according to std::less<value_type>. The merge is stable;
   //!   that is, if an element from *this is equivalent to one from x, then the element
   //!   from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If comparison throws.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   void merge(BOOST_RV_REF(list) x)
   {  this->merge(static_cast<list&>(x)); }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak
   //!   ordering and both *this and x must be sorted according to that ordering
   //!   The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this. The merge is stable; that is, if an element from *this is
   //!   equivalent to one from x, then the element from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If comp throws.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references to *this are not invalidated.
   template <class StrictWeakOrdering>
   void merge(list &x, const StrictWeakOrdering &comp)
   {
      BOOST_ASSERT(this->node_alloc() == x.node_alloc());
      this->icont().merge(x.icont(),
         ValueCompareToNodeCompare<StrictWeakOrdering>(comp));
   }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak
   //!   ordering and both *this and x must be sorted according to that ordering
   //!   The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this. The merge is stable; that is, if an element from *this is
   //!   equivalent to one from x, then the element from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If comp throws.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references to *this are not invalidated.
   template <class StrictWeakOrdering>
   void merge(BOOST_RV_REF(list) x, StrictWeakOrdering comp)
   {  this->merge(static_cast<list&>(x), comp); }

   //! <b>Effects</b>: This function sorts the list *this according to std::less<value_type>.
   //!   The sort is stable, that is, the relative order of equivalent elements is preserved.
   //!
   //! <b>Throws</b>: If comparison throws.
   //!
   //! <b>Notes</b>: Iterators and references are not invalidated.
   //!
   //! <b>Complexity</b>: The number of comparisons is approximately N log N, where N
   //!   is the list's size.
   void sort()
   {  this->sort(value_less());  }

   //! <b>Effects</b>: This function sorts the list *this according to std::less<value_type>.
   //!   The sort is stable, that is, the relative order of equivalent elements is preserved.
   //!
   //! <b>Throws</b>: If comp throws.
   //!
   //! <b>Notes</b>: Iterators and references are not invalidated.
   //!
   //! <b>Complexity</b>: The number of comparisons is approximately N log N, where N
   //!   is the list's size.
   template <class StrictWeakOrdering>
   void sort(StrictWeakOrdering comp)
   {
      // nothing if the list has length 0 or 1.
      if (this->size() < 2)
         return;
      this->icont().sort(ValueCompareToNodeCompare<StrictWeakOrdering>(comp));
   }

   //! <b>Effects</b>: Reverses the order of elements in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: This function is linear time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   void reverse() BOOST_CONTAINER_NOEXCEPT
   {  this->icont().reverse(); }

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const list& x, const list& y)
   {
      if(x.size() != y.size()){
         return false;
      }
      typedef typename list::const_iterator const_iterator;
      const_iterator end1 = x.end();

      const_iterator i1 = x.begin();
      const_iterator i2 = y.begin();
      while (i1 != end1 && *i1 == *i2) {
         ++i1;
         ++i2;
      }
      return i1 == end1;
   }

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const list& x, const list& y)
   {  return !(x == y); }

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const list& x, const list& y)
   {  return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const list& x, const list& y)
   {  return y < x;  }

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const list& x, const list& y)
   {  return !(y < x);  }

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const list& x, const list& y)
   {  return !(x < y);  }

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(list& x, list& y)
   {  x.swap(y);  }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:

   bool priv_try_shrink(size_type new_size)
   {
      const size_type len = this->size();
      if(len > new_size){
         const const_iterator iend = this->cend();
         size_type to_erase = len - new_size;
         const_iterator ifirst;
         if(to_erase < len/2u){
            ifirst = iend;
            while(to_erase--){
               --ifirst;
            }
         }
         else{
            ifirst = this->cbegin();
            size_type to_skip = len - to_erase;
            while(to_skip--){
               ++ifirst;
            }
         }
         this->erase(ifirst, iend);
         return true;
      }
      else{
         return false;
      }
   }

   iterator priv_insert(const_iterator p, const T &x)
   {
      NodePtr tmp = AllocHolder::create_node(x);
      return iterator(this->icont().insert(p.get(), *tmp));
   }

   iterator priv_insert(const_iterator p, BOOST_RV_REF(T) x)
   {
      NodePtr tmp = AllocHolder::create_node(boost::move(x));
      return iterator(this->icont().insert(p.get(), *tmp));
   }

   void priv_push_back (const T &x)
   {  this->insert(this->cend(), x);    }

   void priv_push_back (BOOST_RV_REF(T) x)
   {  this->insert(this->cend(), boost::move(x));    }

   void priv_push_front (const T &x)
   {  this->insert(this->cbegin(), x);  }

   void priv_push_front (BOOST_RV_REF(T) x)
   {  this->insert(this->cbegin(), boost::move(x));  }

   class insertion_functor;
   friend class insertion_functor;

   class insertion_functor
   {
      Icont &icont_;
      typedef typename Icont::const_iterator iconst_iterator;
      const iconst_iterator pos_;

      public:
      insertion_functor(Icont &icont, typename Icont::const_iterator pos)
         :  icont_(icont), pos_(pos)
      {}

      void operator()(Node &n)
      {
         this->icont_.insert(pos_, n);
      }
   };

   //Functors for member algorithm defaults
   struct value_less
   {
      bool operator()(const value_type &a, const value_type &b) const
         {  return a < b;  }
   };

   struct value_equal
   {
      bool operator()(const value_type &a, const value_type &b) const
         {  return a == b;  }
   };
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

};

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}  //namespace container {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator>
struct has_trivial_destructor_after_move<boost::container::list<T, Allocator> >
   : public ::boost::has_trivial_destructor_after_move<Allocator>
{};

namespace container {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}}

#include <boost/container/detail/config_end.hpp>

#endif // BOOST_CONTAINER_LIST_HPP
