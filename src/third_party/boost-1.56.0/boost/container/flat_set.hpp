//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_FLAT_SET_HPP
#define BOOST_CONTAINER_FLAT_SET_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <boost/container/container_fwd.hpp>
#include <utility>
#include <functional>
#include <memory>
#include <boost/container/detail/flat_tree.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/move/utility.hpp>
#include <boost/move/detail/move_helpers.hpp>

namespace boost {
namespace container {

//! flat_set is a Sorted Associative Container that stores objects of type Key.
//! It is also a Unique Associative Container, meaning that no two elements are the same.
//!
//! flat_set is similar to std::set but it's implemented like an ordered vector.
//! This means that inserting a new element into a flat_set invalidates
//! previous iterators and references
//!
//! Erasing an element of a flat_set invalidates iterators and references
//! pointing to elements that come after (their keys are bigger) the erased element.
//!
//! This container provides random-access iterators.
//!
//! \tparam Key is the type to be inserted in the set, which is also the key_type
//! \tparam Compare is the comparison functor used to order keys
//! \tparam Allocator is the allocator to be used to allocate memory for this container
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class Compare = std::less<Key>, class Allocator = std::allocator<Key> >
#else
template <class Key, class Compare, class Allocator>
#endif
class flat_set
   ///@cond
   : public container_detail::flat_tree<Key, Key, container_detail::identity<Key>, Compare, Allocator>
   ///@endcond
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_set)
   typedef container_detail::flat_tree<Key, Key, container_detail::identity<Key>, Compare, Allocator> base_t;
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

   public:
   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Default constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   explicit flat_set()
      : base_t()
   {}

   //! <b>Effects</b>: Constructs an empty container using the specified
   //! comparison object and allocator.
   //!
   //! <b>Complexity</b>: Constant.
   explicit flat_set(const Compare& comp,
                     const allocator_type& a = allocator_type())
      : base_t(comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty container using the specified allocator.
   //!
   //! <b>Complexity</b>: Constant.
   explicit flat_set(const allocator_type& a)
      : base_t(a)
   {}

   //! <b>Effects</b>: Constructs an empty container using the specified comparison object and
   //! allocator, and inserts elements from the range [first ,last ).
   //!
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   flat_set(InputIterator first, InputIterator last,
            const Compare& comp = Compare(),
            const allocator_type& a = allocator_type())
      : base_t(true, first, last, comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty container using the specified comparison object and
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
   flat_set(ordered_unique_range_t, InputIterator first, InputIterator last,
            const Compare& comp = Compare(),
            const allocator_type& a = allocator_type())
      : base_t(ordered_range, first, last, comp, a)
   {}

   //! <b>Effects</b>: Copy constructs the container.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   flat_set(const flat_set& x)
      : base_t(static_cast<const base_t&>(x))
   {}

   //! <b>Effects</b>: Move constructs thecontainer. Constructs *this using mx's resources.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Postcondition</b>: mx is emptied.
   flat_set(BOOST_RV_REF(flat_set) mx)
      : base_t(boost::move(static_cast<base_t&>(mx)))
   {}

   //! <b>Effects</b>: Copy constructs a container using the specified allocator.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   flat_set(const flat_set& x, const allocator_type &a)
      : base_t(static_cast<const base_t&>(x), a)
   {}

   //! <b>Effects</b>: Move constructs a container using the specified allocator.
   //!                 Constructs *this using mx's resources.
   //!
   //! <b>Complexity</b>: Constant if a == mx.get_allocator(), linear otherwise
   flat_set(BOOST_RV_REF(flat_set) mx, const allocator_type &a)
      : base_t(boost::move(static_cast<base_t&>(mx)), a)
   {}

   //! <b>Effects</b>: Makes *this a copy of x.
   //!
   //! <b>Complexity</b>: Linear in x.size().
   flat_set& operator=(BOOST_COPY_ASSIGN_REF(flat_set) x)
   {  return static_cast<flat_set&>(this->base_t::operator=(static_cast<const base_t&>(x)));  }

   //! <b>Throws</b>: If allocator_traits_type::propagate_on_container_move_assignment
   //!   is false and (allocation throws or value_type's move constructor throws)
   //!
   //! <b>Complexity</b>: Constant if allocator_traits_type::
   //!   propagate_on_container_move_assignment is true or
   //!   this->get>allocator() == x.get_allocator(). Linear otherwise.
   flat_set& operator=(BOOST_RV_REF(flat_set) x)
      BOOST_CONTAINER_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value)
   {  return static_cast<flat_set&>(this->base_t::operator=(boost::move(static_cast<base_t&>(x))));  }

   #ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //!
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator begin() BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator end() BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   bool empty() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type size() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type max_size() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type capacity() const BOOST_CONTAINER_NOEXCEPT;

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //!
   //! <b>Throws</b>: If memory allocation allocation throws or Key's copy constructor throws.
   //!
   //! <b>Note</b>: If capacity() is less than "cnt", iterators and references to
   //!   to values might be invalidated.
   void reserve(size_type cnt);

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //    with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or Key's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit();

   #endif   //   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object x of type Key constructed with
   //!   std::forward<Args>(args)... if and only if there is no element in the container
   //!   with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class... Args>
   std::pair<iterator,bool> emplace(Args&&... args)
   {  return this->base_t::emplace_unique(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)... in the container if and only if there is
   //!   no element in the container with key equivalent to the key of x.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
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
   {  return this->base_t::emplace_hint_unique                                                     \
            (hint BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }               \
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
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   std::pair<iterator, bool> insert(const value_type &x);

   //! <b>Effects</b>: Inserts a new value_type move constructed from the pair if and
   //! only if there is no element in the container with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
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
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator p, const value_type &x);

   //! <b>Effects</b>: Inserts an element move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
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
   //!   search time plus N*size() insertion time.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last)
      {  this->base_t::insert_unique(first, last);  }

   //! <b>Requires</b>: first, last are not iterators into *this and
   //! must be ordered according to the predicate and must be
   //! unique values.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) .This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   //!   search time plus N*size() insertion time.
   //!
   //! <b>Note</b>: Non-standard extension. If an element is inserted it might invalidate elements.
   template <class InputIterator>
   void insert(ordered_unique_range_t, InputIterator first, InputIterator last)
      {  this->base_t::insert_unique(ordered_unique_range, first, last);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Erases the element pointed to by position.
   //!
   //! <b>Returns</b>: Returns an iterator pointing to the element immediately
   //!   following q prior to the element being erased. If no such element exists,
   //!   returns end().
   //!
   //! <b>Complexity</b>: Linear to the elements with keys bigger than position
   //!
   //! <b>Note</b>: Invalidates elements with keys
   //!   not less than the erased element.
   iterator erase(const_iterator position);

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   size_type erase(const key_type& x);

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: size()*N where N is the distance from first to last.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   iterator erase(const_iterator first, const_iterator last);

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(flat_set& x);

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear() BOOST_CONTAINER_NOEXCEPT;

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

   #endif   //   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const
   {  return static_cast<size_type>(this->base_t::find(x) != this->base_t::cend());  }

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

   #endif   //   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator, const_iterator> equal_range(const key_type& x) const
   {  return this->base_t::lower_bound_range(x);  }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x)
   {  return this->base_t::lower_bound_range(x);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const flat_set& x, const flat_set& y);

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(flat_set& x, flat_set& y);

   #endif   //#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   template<class KeyType>
   std::pair<iterator, bool> priv_insert(BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_unique(::boost::forward<KeyType>(x));  }

   template<class KeyType>
   iterator priv_insert(const_iterator p, BOOST_FWD_REF(KeyType) x)
   {  return this->base_t::insert_unique(p, ::boost::forward<KeyType>(x)); }
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}  //namespace container {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class Key, class C, class Allocator>
struct has_trivial_destructor_after_move<boost::container::flat_set<Key, C, Allocator> >
{
   static const bool value = has_trivial_destructor_after_move<Allocator>::value &&has_trivial_destructor_after_move<C>::value;
};

namespace container {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//! flat_multiset is a Sorted Associative Container that stores objects of type Key.
//!
//! flat_multiset can store multiple copies of the same key value.
//!
//! flat_multiset is similar to std::multiset but it's implemented like an ordered vector.
//! This means that inserting a new element into a flat_multiset invalidates
//! previous iterators and references
//!
//! Erasing an element invalidates iterators and references
//! pointing to elements that come after (their keys are bigger) the erased element.
//!
//! This container provides random-access iterators.
//!
//! \tparam Key is the type to be inserted in the multiset, which is also the key_type
//! \tparam Compare is the comparison functor used to order keys
//! \tparam Allocator is the allocator to be used to allocate memory for this container
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class Compare = std::less<Key>, class Allocator = std::allocator<Key> >
#else
template <class Key, class Compare, class Allocator>
#endif
class flat_multiset
   ///@cond
   : public container_detail::flat_tree<Key, Key, container_detail::identity<Key>, Compare, Allocator>
   ///@endcond
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_multiset)
   typedef container_detail::flat_tree<Key, Key, container_detail::identity<Key>, Compare, Allocator> base_t;
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

   //! @copydoc ::boost::container::flat_set::flat_set()
   explicit flat_multiset()
      : base_t()
   {}

   //! @copydoc ::boost::container::flat_set::flat_set(const Compare&, const allocator_type&)
   explicit flat_multiset(const Compare& comp,
                          const allocator_type& a = allocator_type())
      : base_t(comp, a)
   {}

   //! @copydoc ::boost::container::flat_set::flat_set(const allocator_type&)
   explicit flat_multiset(const allocator_type& a)
      : base_t(a)
   {}

   //! @copydoc ::boost::container::flat_set::flat_set(InputIterator, InputIterator, const Compare& comp, const allocator_type&)
   template <class InputIterator>
   flat_multiset(InputIterator first, InputIterator last,
                 const Compare& comp        = Compare(),
                 const allocator_type& a = allocator_type())
      : base_t(false, first, last, comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty flat_multiset using the specified comparison object and
   //! allocator, and inserts elements from the ordered range [first ,last ). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate.
   //!
   //! <b>Complexity</b>: Linear in N.
   //!
   //! <b>Note</b>: Non-standard extension.
   template <class InputIterator>
   flat_multiset(ordered_range_t, InputIterator first, InputIterator last,
                 const Compare& comp        = Compare(),
                 const allocator_type& a = allocator_type())
      : base_t(ordered_range, first, last, comp, a)
   {}

   //! @copydoc ::boost::container::flat_set::flat_set(const flat_set &)
   flat_multiset(const flat_multiset& x)
      : base_t(static_cast<const base_t&>(x))
   {}

   //! @copydoc ::boost::container::flat_set(flat_set &&)
   flat_multiset(BOOST_RV_REF(flat_multiset) mx)
      : base_t(boost::move(static_cast<base_t&>(mx)))
   {}

   //! @copydoc ::boost::container::flat_set(const flat_set &, const allocator_type &)
   flat_multiset(const flat_multiset& x, const allocator_type &a)
      : base_t(static_cast<const base_t&>(x), a)
   {}

   //! @copydoc ::boost::container::flat_set(flat_set &&, const allocator_type &)
   flat_multiset(BOOST_RV_REF(flat_multiset) mx, const allocator_type &a)
      : base_t(boost::move(static_cast<base_t&>(mx)), a)
   {}

   //! @copydoc ::boost::container::flat_set::operator=(const flat_set &)
   flat_multiset& operator=(BOOST_COPY_ASSIGN_REF(flat_multiset) x)
   {  return static_cast<flat_multiset&>(this->base_t::operator=(static_cast<const base_t&>(x)));  }

   //! @copydoc ::boost::container::flat_set::operator=(flat_set &&)
   flat_multiset& operator=(BOOST_RV_REF(flat_multiset) mx)
      BOOST_CONTAINER_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value)
   {  return static_cast<flat_multiset&>(this->base_t::operator=(boost::move(static_cast<base_t&>(mx))));  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! @copydoc ::boost::container::flat_set::get_allocator()
   allocator_type get_allocator() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::get_stored_allocator()
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::get_stored_allocator() const
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::begin()
   iterator begin() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::begin() const
   const_iterator begin() const;

   //! @copydoc ::boost::container::flat_set::cbegin() const
   const_iterator cbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::end()
   iterator end() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::end() const
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::cend() const
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::rbegin()
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::rbegin() const
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::crbegin() const
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::rend()
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::rend() const
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::crend() const
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::empty() const
   bool empty() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::size() const
   size_type size() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::max_size() const
   size_type max_size() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::capacity() const
   size_type capacity() const BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::reserve(size_type)
   void reserve(size_type cnt);

   //! @copydoc ::boost::container::flat_set::shrink_to_fit()
   void shrink_to_fit();

   #endif   //   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)... and returns the iterator pointing to the
   //!   newly inserted element.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class... Args>
   iterator emplace(Args&&... args)
   {  return this->base_t::emplace_equal(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type Key constructed with
   //!   std::forward<Args>(args)... in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {  return this->base_t::emplace_hint_equal(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                            \
   {  return this->base_t::emplace_equal(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }   \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return this->base_t::emplace_hint_equal                                                        \
            (hint BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }               \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts x and returns the iterator pointing to the
   //!   newly inserted element.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const value_type &x);

   //! <b>Effects</b>: Inserts a new value_type move constructed from x
   //!   and returns the iterator pointing to the newly inserted element.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
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
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator p, const value_type &x);

   //! <b>Effects</b>: Inserts a new value move constructed  from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, value_type &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, value_type, iterator, this->priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: first, last are not iterators into *this.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) .
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   //!   search time plus N*size() insertion time.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last)
      {  this->base_t::insert_equal(first, last);  }

   //! <b>Requires</b>: first, last are not iterators into *this and
   //! must be ordered according to the predicate.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) .This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   //!   search time plus N*size() insertion time.
   //!
   //! <b>Note</b>: Non-standard extension. If an element is inserted it might invalidate elements.
   template <class InputIterator>
   void insert(ordered_range_t, InputIterator first, InputIterator last)
      {  this->base_t::insert_equal(ordered_range, first, last);  }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! @copydoc ::boost::container::flat_set::erase(const_iterator)
   iterator erase(const_iterator position);

   //! @copydoc ::boost::container::flat_set::erase(const key_type&)
   size_type erase(const key_type& x);

   //! @copydoc ::boost::container::flat_set::erase(const_iterator,const_iterator)
   iterator erase(const_iterator first, const_iterator last);

   //! @copydoc ::boost::container::flat_set::swap
   void swap(flat_multiset& x);

   //! @copydoc ::boost::container::flat_set::clear
   void clear() BOOST_CONTAINER_NOEXCEPT;

   //! @copydoc ::boost::container::flat_set::key_comp
   key_compare key_comp() const;

   //! @copydoc ::boost::container::flat_set::value_comp
   value_compare value_comp() const;

   //! @copydoc ::boost::container::flat_set::find(const key_type& )
   iterator find(const key_type& x);

   //! @copydoc ::boost::container::flat_set::find(const key_type& ) const
   const_iterator find(const key_type& x) const;

   //! @copydoc ::boost::container::flat_set::count(const key_type& ) const
   size_type count(const key_type& x) const;

   //! @copydoc ::boost::container::flat_set::lower_bound(const key_type& )
   iterator lower_bound(const key_type& x);

   //! @copydoc ::boost::container::flat_set::lower_bound(const key_type& ) const
   const_iterator lower_bound(const key_type& x) const;

   //! @copydoc ::boost::container::flat_set::upper_bound(const key_type& )
   iterator upper_bound(const key_type& x);

   //! @copydoc ::boost::container::flat_set::upper_bound(const key_type& ) const
   const_iterator upper_bound(const key_type& x) const;

   //! @copydoc ::boost::container::flat_set::equal_range(const key_type& ) const
   std::pair<const_iterator, const_iterator> equal_range(const key_type& x) const;

   //! @copydoc ::boost::container::flat_set::equal_range(const key_type& )
   std::pair<iterator,iterator> equal_range(const key_type& x);

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const flat_multiset& x, const flat_multiset& y);

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(flat_multiset& x, flat_multiset& y);

   #endif   //#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED

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
template <class Key, class C, class Allocator>
struct has_trivial_destructor_after_move<boost::container::flat_multiset<Key, C, Allocator> >
{
   static const bool value = has_trivial_destructor_after_move<Allocator>::value && has_trivial_destructor_after_move<C>::value;
};

namespace container {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

}}

#include <boost/container/detail/config_end.hpp>

#endif /* BOOST_CONTAINER_FLAT_SET_HPP */
