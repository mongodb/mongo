//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_SET_HPP
#define BOOST_CONTAINER_SET_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/container_fwd.hpp>

#include <utility>
#include <functional>
#include <memory>

#include <boost/move/utility.hpp>
#include <boost/move/detail/move_helpers.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/tree.hpp>
#include <boost/move/utility.hpp>
#ifndef BOOST_CONTAINER_PERFECT_FORWARDING
#include <boost/container/detail/preprocessor.hpp>
#endif

namespace boost {
namespace container {

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED

//! A set is a kind of associative container that supports unique keys (contains at
//! most one of each key value) and provides for fast retrieval of the keys themselves.
//! Class set supports bidirectional iterators.
//!
//! A set satisfies all of the requirements of a container and of a reversible container
//! , and of an associative container. A set also provides most operations described in
//! for unique keys.
//!
//! \tparam Key is the type to be inserted in the set, which is also the key_type
//! \tparam Compare is the comparison functor used to order keys
//! \tparam Allocator is the allocator to be used to allocate memory for this container
//! \tparam SetOptions is an packed option type generated using using boost::container::tree_assoc_options.
template <class Key, class Compare = std::less<Key>, class Allocator = std::allocator<Key>, class SetOptions = tree_assoc_defaults >
#else
template <class Key, class Compare, class Allocator, class SetOptions>
#endif
class set
   ///@cond
   : public container_detail::tree
      < Key, Key, container_detail::identity<Key>, Compare, Allocator, SetOptions>
   ///@endcond
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(set)
   typedef container_detail::tree
      < Key, Key, container_detail::identity<Key>, Compare, Allocator, SetOptions> base_t;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   //////////////////////////////////////////////
   //
   //                    types
   //
   //////////////////////////////////////////////
   typedef Key                                                                         key_type;
   typedef Key                                                                         value_type;
   typedef Compare                                                                     key_compare;
   typedef Compare                                                                     value_compare;
   typedef ::boost::container::allocator_traits<Allocator>                             allocator_traits_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer           pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_pointer     const_pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::reference         reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_reference   const_reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::size_type         size_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::difference_type   difference_type;
   typedef Allocator                                                                   allocator_type;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::stored_allocator_type)              stored_allocator_type;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::iterator)                           iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::const_iterator)                     const_iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::reverse_iterator)                   reverse_iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::const_reverse_iterator)             const_reverse_iterator;

   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Default constructs an empty set.
   //!
   //! <b>Complexity</b>: Constant.
   set()
      : base_t()
   {}

   //! <b>Effects</b>: Constructs an empty set using the specified comparison object
   //! and allocator.
   //!
   //! <b>Complexity</b>: Constant.
   explicit set(const Compare& comp,
                const allocator_type& a = allocator_type())
      : base_t(comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty set using the specified allocator object.
   //!
   //! <b>Complexity</b>: Constant.
   explicit set(const allocator_type& a)
      : base_t(a)
   {}

   //! <b>Effects</b>: Constructs an empty set using the specified comparison object and
   //! allocator, and inserts elements from the range [first ,last ).
   //!
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   set(InputIterator first, InputIterator last, const Compare& comp = Compare(),
         const allocator_type& a = allocator_type())
      : base_t(true, first, last, comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty set using the specified comparison object and
   //! allocator, and inserts elements from the ordered unique range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate and must be
   //! unique values.
   //!
   //! <b>Complexity</b>: Linear in N.
   //!
   //! <b>Note</b>: Non-standard extension.
   template <class InputIterator>
   set( ordered_unique_range_t, InputIterator first, InputIterator last
      , const Compare& comp = Compare(), const allocator_type& a = allocator_type())
      : base_t(ordered_range, first, last, comp, a)
   {}

   //! <b>Effects</b>: Copy constructs a set.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   set(const set& x)
      : base_t(static_cast<const base_t&>(x))
   {}

   //! <b>Effects</b>: Move constructs a set. Constructs *this using x's resources.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Postcondition</b>: x is emptied.
   set(BOOST_RV_REF(set) x)
      : base_t(boost::move(static_cast<base_t&>(x)))
   {}

   //! <b>Effects</b>: Copy constructs a set using the specified allocator.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   set(const set& x, const allocator_type &a)
      : base_t(static_cast<const base_t&>(x), a)
   {}

   //! <b>Effects</b>: Move constructs a set using the specified allocator.
   //!                 Constructs *this using x's resources.
   //!
   //! <b>Complexity</b>: Constant if a == x.get_allocator(), linear otherwise.
   set(BOOST_RV_REF(set) x, const allocator_type &a)
      : base_t(boost::move(static_cast<base_t&>(x)), a)
   {}

   //! <b>Effects</b>: Makes *this a copy of x.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   set& operator=(BOOST_COPY_ASSIGN_REF(set) x)
   {  return static_cast<set&>(this->base_t::operator=(static_cast<const base_t&>(x)));  }

   //! <b>Effects</b>: this->swap(x.get()).
   //!
   //! <b>Throws</b>: If allocator_traits_type::propagate_on_container_move_assignment
   //!   is false and (allocation throws or value_type's move constructor throws)
   //!
   //! <b>Complexity</b>: Constant if allocator_traits_type::
   //!   propagate_on_container_move_assignment is true or
   //!   this->get>allocator() == x.get_allocator(). Linear otherwise.
   set& operator=(BOOST_RV_REF(set) x)
      BOOST_CONTAINER_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value)
   {  return static_cast<set&>(this->base_t::operator=(boost::move(static_cast<base_t&>(x))));  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //!
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const;

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator();

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const;

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant
   iterator begin();

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const;

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const;

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator end();

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator end() const;

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin();

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend();

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const;

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   bool empty() const;

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type size() const;

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type max_size() const;
   #endif   //   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>:  Inserts an object x of type Key constructed with
   //!   std::forward<Args>(args)... if and only if there is
   //!   no element in the container with equivalent value.
   //!   and returns the iterator pointing to the
   //!   newly inserted element.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   Key's in-place constructor throws.
   //!
   //! <b>Complexity</b>: Logarithmic.
   template <class... Args>
   std::pair<iterator,bool> emplace(Args&&... args)
   {  return this->base_t::emplace_unique(boost::forward<Args>(args)...); }

   //! <b>Effects</b>:  Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)... if and only if there is
   //!   no element in the container with equivalent value.
   //!   p is a hint pointing to where the insert
   //!   should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {  return this->base_t::emplace_hint_unique(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   std::pair<iterator,bool> emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))            \
   {  return this->base_t::emplace_unique(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }\
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return this->base_t::emplace_hint_unique(hint                                                \
                               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));}   \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts x if and only if there is no element in the container
   //!   with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator, bool> insert(const value_type &x);

   //! <b>Effects</b>: Move constructs a new value from x if and only if there is
   //!   no element in the container with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator, bool> insert(value_type &&x);
   #else
   private:
   typedef std::pair<iterator, bool> insert_return_pair;
   public:
   BOOST_MOVE_CONVERSION_AWARE_CATCH(insert, value_type, insert_return_pair, this->priv_insert)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x in the container if and only if there is
   //!   no element in the container with key equivalent to the key of x.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(const_iterator p, const value_type &x);

   //! <b>Effects</b>: Inserts an element move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(const_iterator position, value_type &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, value_type, iterator, this->priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: first, last are not iterators into *this.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) if and only
   //!   if there is no element with key equivalent to the key of that element.
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last)
   {  this->base_t::insert_unique(first, last);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Erases the element pointed to by p.
   //!
   //! <b>Returns</b>: Returns an iterator pointing to the element immediately
   //!   following q prior to the element being erased. If no such element exists,
   //!   returns end().
   //!
   //! <b>Complexity</b>: Amortized constant time
   iterator erase(const_iterator p);

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: log(size()) + count(k)
   size_type erase(const key_type& x);

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: log(size())+N where N is the distance from first to last.
   iterator erase(const_iterator first, const_iterator last);

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(set& x);

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear();

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //!
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const;

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //!
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const;

   //! <b>Returns</b>: An iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator find(const key_type& x);

   //! <b>Returns</b>: Allocator const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   const_iterator find(const key_type& x) const;

   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const
   {  return static_cast<size_type>(this->base_t::find(x) != this->base_t::cend());  }

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x)
   {  return static_cast<size_type>(this->base_t::find(x) != this->base_t::end());  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator lower_bound(const key_type& x);

   //! <b>Returns</b>: Allocator const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const;

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x);

   //! <b>Returns</b>: Allocator const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const;

   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x)
   {  return this->base_t::lower_bound_range(x);  }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator, const_iterator> equal_range(const key_type& x) const
   {  return this->base_t::lower_bound_range(x);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x);

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator, const_iterator> equal_range(const key_type& x) const;

   //! <b>Effects</b>: Rebalances the tree. It's a no-op for Red-Black and AVL trees.
   //!
   //! <b>Complexity</b>: Linear
   void rebalance();

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const set& x, const set& y);

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const set& x, const set& y);

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const set& x, const set& y);

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const set& x, const set& y);

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const set& x, const set& y);

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const set& x, const set& y);

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(set& x, set& y);

   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   template <class KeyType>
   std::pair<iterator, bool> priv_insert(BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_unique(::boost::forward<KeyType>(x));  }

   template <class KeyType>
   iterator priv_insert(const_iterator p, BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_unique(p, ::boost::forward<KeyType>(x)); }
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}  //namespace container {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class Key, class C, class SetOptions, class Allocator>
struct has_trivial_destructor_after_move<boost::container::set<Key, C, Allocator, SetOptions> >
{
   static const bool value = has_trivial_destructor_after_move<Allocator>::value && has_trivial_destructor_after_move<C>::value;
};

namespace container {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED

//! A multiset is a kind of associative container that supports equivalent keys
//! (possibly contains multiple copies of the same key value) and provides for
//! fast retrieval of the keys themselves. Class multiset supports bidirectional iterators.
//!
//! A multiset satisfies all of the requirements of a container and of a reversible
//! container, and of an associative container). multiset also provides most operations
//! described for duplicate keys.
//!
//! \tparam Key is the type to be inserted in the set, which is also the key_type
//! \tparam Compare is the comparison functor used to order keys
//! \tparam Allocator is the allocator to be used to allocate memory for this container
//! \tparam MultiSetOptions is an packed option type generated using using boost::container::tree_assoc_options.
template <class Key, class Compare = std::less<Key>, class Allocator = std::allocator<Key>, class MultiSetOptions = tree_assoc_defaults >
#else
template <class Key, class Compare, class Allocator, class MultiSetOptions>
#endif
class multiset
   /// @cond
   : public container_detail::tree
      <Key, Key,container_detail::identity<Key>, Compare, Allocator, MultiSetOptions>
   /// @endcond
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(multiset)
   typedef container_detail::tree
      <Key, Key,container_detail::identity<Key>, Compare, Allocator, MultiSetOptions> base_t;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:

   //////////////////////////////////////////////
   //
   //                    types
   //
   //////////////////////////////////////////////
   typedef Key                                                                         key_type;
   typedef Key                                                                         value_type;
   typedef Compare                                                                     key_compare;
   typedef Compare                                                                     value_compare;
   typedef ::boost::container::allocator_traits<Allocator>                             allocator_traits_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer           pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_pointer     const_pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::reference         reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_reference   const_reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::size_type         size_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::difference_type   difference_type;
   typedef Allocator                                                                   allocator_type;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::stored_allocator_type)              stored_allocator_type;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::iterator)                           iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::const_iterator)                     const_iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::reverse_iterator)                   reverse_iterator;
   typedef typename BOOST_CONTAINER_IMPDEF(base_t::const_reverse_iterator)             const_reverse_iterator;

   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! @copydoc ::boost::container::set::set()
   multiset()
      : base_t()
   {}

   //! @copydoc ::boost::container::set::set(const Compare&, const allocator_type&)
   explicit multiset(const Compare& comp,
                     const allocator_type& a = allocator_type())
      : base_t(comp, a)
   {}

   //! @copydoc ::boost::container::set::set(const allocator_type&)
   explicit multiset(const allocator_type& a)
      : base_t(a)
   {}

   //! @copydoc ::boost::container::set::set(InputIterator, InputIterator, const Compare& comp, const allocator_type&)
   template <class InputIterator>
   multiset(InputIterator first, InputIterator last,
            const Compare& comp = Compare(),
            const allocator_type& a = allocator_type())
      : base_t(false, first, last, comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty multiset using the specified comparison object and
   //! allocator, and inserts elements from the ordered range [first ,last ). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate.
   //!
   //! <b>Complexity</b>: Linear in N.
   //!
   //! <b>Note</b>: Non-standard extension.
   template <class InputIterator>
   multiset( ordered_range_t, InputIterator first, InputIterator last
           , const Compare& comp = Compare()
           , const allocator_type& a = allocator_type())
      : base_t(ordered_range, first, last, comp, a)
   {}

   //! @copydoc ::boost::container::set::set(const set &)
   multiset(const multiset& x)
      : base_t(static_cast<const base_t&>(x))
   {}

   //! @copydoc ::boost::container::set(set &&)
   multiset(BOOST_RV_REF(multiset) x)
      : base_t(boost::move(static_cast<base_t&>(x)))
   {}

   //! @copydoc ::boost::container::set(const set &, const allocator_type &)
   multiset(const multiset& x, const allocator_type &a)
      : base_t(static_cast<const base_t&>(x), a)
   {}

   //! @copydoc ::boost::container::set(set &&, const allocator_type &)
   multiset(BOOST_RV_REF(multiset) x, const allocator_type &a)
      : base_t(boost::move(static_cast<base_t&>(x)), a)
   {}

   //! @copydoc ::boost::container::set::operator=(const set &)
   multiset& operator=(BOOST_COPY_ASSIGN_REF(multiset) x)
   {  return static_cast<multiset&>(this->base_t::operator=(static_cast<const base_t&>(x)));  }

   //! @copydoc ::boost::container::set::operator=(set &&)
   multiset& operator=(BOOST_RV_REF(multiset) x)
   {  return static_cast<multiset&>(this->base_t::operator=(boost::move(static_cast<base_t&>(x))));  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! @copydoc ::boost::container::set::get_allocator()
   allocator_type get_allocator() const;

   //! @copydoc ::boost::container::set::get_stored_allocator()
   stored_allocator_type &get_stored_allocator();

   //! @copydoc ::boost::container::set::get_stored_allocator() const
   const stored_allocator_type &get_stored_allocator() const;

   //! @copydoc ::boost::container::set::begin()
   iterator begin();

   //! @copydoc ::boost::container::set::begin() const
   const_iterator begin() const;

   //! @copydoc ::boost::container::set::cbegin() const
   const_iterator cbegin() const;

   //! @copydoc ::boost::container::set::end()
   iterator end() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::end() const
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::cend() const
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::rbegin()
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::rbegin() const
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::crbegin() const
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::rend()
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::rend() const
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::crend() const
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::empty() const
   bool empty() const;

   //! @copydoc ::boost::container::set::size() const
   size_type size() const;

   //! @copydoc ::boost::container::set::max_size() const
   size_type max_size() const;

   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)... and returns the iterator pointing to the
   //!   newly inserted element.
   //!
   //! <b>Complexity</b>: Logarithmic.
   template <class... Args>
   iterator emplace(Args&&... args)
   {  return this->base_t::emplace_equal(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)...
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {  return this->base_t::emplace_hint_equal(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                            \
   {  return this->base_t::emplace_equal(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); } \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return this->base_t::emplace_hint_equal(hint                                                 \
                               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));}   \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts x and returns the iterator pointing to the
   //!   newly inserted element.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(const value_type &x);

   //! <b>Effects</b>: Inserts a copy of x in the container.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(value_type &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(insert, value_type, iterator, this->priv_insert)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(const_iterator p, const value_type &x);

   //! <b>Effects</b>: Inserts a value move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(const_iterator position, value_type &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, value_type, iterator, this->priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: first, last are not iterators into *this.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) .
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last)
   {  this->base_t::insert_equal(first, last);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! @copydoc ::boost::container::set::erase(const_iterator)
   iterator erase(const_iterator p);

   //! @copydoc ::boost::container::set::erase(const key_type&)
   size_type erase(const key_type& x);

   //! @copydoc ::boost::container::set::erase(const_iterator,const_iterator)
   iterator erase(const_iterator first, const_iterator last);

   //! @copydoc ::boost::container::set::swap
   void swap(flat_multiset& x);

   //! @copydoc ::boost::container::set::clear
   void clear() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::set::key_comp
   key_compare key_comp() const;

   //! @copydoc ::boost::container::set::value_comp
   value_compare value_comp() const;

   //! @copydoc ::boost::container::set::find(const key_type& )
   iterator find(const key_type& x);

   //! @copydoc ::boost::container::set::find(const key_type& ) const
   const_iterator find(const key_type& x) const;

   //! @copydoc ::boost::container::set::count(const key_type& ) const
   size_type count(const key_type& x) const;

   //! @copydoc ::boost::container::set::lower_bound(const key_type& )
   iterator lower_bound(const key_type& x);

   //! @copydoc ::boost::container::set::lower_bound(const key_type& ) const
   const_iterator lower_bound(const key_type& x) const;

   //! @copydoc ::boost::container::set::upper_bound(const key_type& )
   iterator upper_bound(const key_type& x);

   //! @copydoc ::boost::container::set::upper_bound(const key_type& ) const
   const_iterator upper_bound(const key_type& x) const;

   //! @copydoc ::boost::container::set::equal_range(const key_type& ) const
   std::pair<const_iterator, const_iterator> equal_range(const key_type& x) const;

   //! @copydoc ::boost::container::set::equal_range(const key_type& )
   std::pair<iterator,iterator> equal_range(const key_type& x);

   //! @copydoc ::boost::container::set::rebalance()
   void rebalance();

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const multiset& x, const multiset& y);

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const multiset& x, const multiset& y);

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const multiset& x, const multiset& y);

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const multiset& x, const multiset& y);

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const multiset& x, const multiset& y);

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const multiset& x, const multiset& y);

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(multiset& x, multiset& y);

   #endif   //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   template <class KeyType>
   iterator priv_insert(BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_equal(::boost::forward<KeyType>(x));  }

   template <class KeyType>
   iterator priv_insert(const_iterator p, BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_equal(p, ::boost::forward<KeyType>(x)); }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}  //namespace container {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class Key, class C, class Allocator, class MultiSetOptions>
struct has_trivial_destructor_after_move<boost::container::multiset<Key, C, Allocator, MultiSetOptions> >
{
   static const bool value = has_trivial_destructor_after_move<Allocator>::value && has_trivial_destructor_after_move<C>::value;
};

namespace container {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}}

#include <boost/container/detail/config_end.hpp>

#endif /* BOOST_CONTAINER_SET_HPP */

