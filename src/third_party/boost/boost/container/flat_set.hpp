//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_FLAT_SET_HPP
#define BOOST_CONTAINER_FLAT_SET_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
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
#include <boost/move/move.hpp>

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
namespace boost {
namespace container {
#else
namespace boost {
namespace container {
#endif

/// @cond
// Forward declarations of operators < and ==, needed for friend declaration.

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Pred = std::less<T>, class A = std::allocator<T> >
#else
template <class T, class Pred, class A>
#endif
class flat_set;

template <class T, class Pred, class A>
inline bool operator==(const flat_set<T,Pred,A>& x, 
                       const flat_set<T,Pred,A>& y);

template <class T, class Pred, class A>
inline bool operator<(const flat_set<T,Pred,A>& x, 
                      const flat_set<T,Pred,A>& y);
/// @endcond

//! flat_set is a Sorted Associative Container that stores objects of type Key. 
//! flat_set is a Simple Associative Container, meaning that its value type, 
//! as well as its key type, is Key. It is also a Unique Associative Container, 
//! meaning that no two elements are the same. 
//! 
//! flat_set is similar to std::set but it's implemented like an ordered vector.
//! This means that inserting a new element into a flat_set invalidates
//! previous iterators and references
//!
//! Erasing an element of a flat_set invalidates iterators and references 
//! pointing to elements that come after (their keys are bigger) the erased element.
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Pred = std::less<T>, class A = std::allocator<T> >
#else
template <class T, class Pred, class A>
#endif
class flat_set 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_set)
   typedef container_detail::flat_tree<T, T, container_detail::identity<T>, Pred, A> tree_t;
   tree_t m_flat_tree;  // flat tree representing flat_set
   typedef typename container_detail::
      move_const_ref_type<T>::type insert_const_ref_type;
   /// @endcond

   public:

   // typedefs:
   typedef typename tree_t::key_type               key_type;
   typedef typename tree_t::value_type             value_type;
   typedef typename tree_t::pointer                pointer;
   typedef typename tree_t::const_pointer          const_pointer;
   typedef typename tree_t::reference              reference;
   typedef typename tree_t::const_reference        const_reference;
   typedef typename tree_t::key_compare            key_compare;
   typedef typename tree_t::value_compare          value_compare;
   typedef typename tree_t::iterator               iterator;
   typedef typename tree_t::const_iterator         const_iterator;
   typedef typename tree_t::reverse_iterator       reverse_iterator;
   typedef typename tree_t::const_reverse_iterator const_reverse_iterator;
   typedef typename tree_t::size_type              size_type;
   typedef typename tree_t::difference_type        difference_type;
   typedef typename tree_t::allocator_type         allocator_type;
   typedef typename tree_t::stored_allocator_type  stored_allocator_type;

   //! <b>Effects</b>: Defatuls constructs an empty flat_map.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit flat_set()
      : m_flat_tree()
   {}

   //! <b>Effects</b>: Constructs an empty flat_map using the specified
   //! comparison object and allocator.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit flat_set(const Pred& comp,
                     const allocator_type& a = allocator_type())
      : m_flat_tree(comp, a)
   {}

   //! <b>Effects</b>: Constructs an empty map using the specified comparison object and 
   //! allocator, and inserts elements from the range [first ,last ).
   //! 
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using 
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   flat_set(InputIterator first, InputIterator last, 
            const Pred& comp = Pred(),
            const allocator_type& a = allocator_type())
      : m_flat_tree(comp, a) 
      { m_flat_tree.insert_unique(first, last); }

   //! <b>Effects</b>: Constructs an empty flat_set using the specified comparison object and 
   //! allocator, and inserts elements from the ordered unique range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate and must be
   //! unique values.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   flat_set(ordered_unique_range_t, InputIterator first, InputIterator last, 
            const Pred& comp = Pred(),
            const allocator_type& a = allocator_type())
      : m_flat_tree(ordered_range, first, last, comp, a) 
   {}

   //! <b>Effects</b>: Copy constructs a map.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_set(const flat_set<T,Pred,A>& x) 
      : m_flat_tree(x.m_flat_tree) {}

   //! <b>Effects</b>: Move constructs a map. Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   flat_set(BOOST_RV_REF(flat_set) mx) 
      : m_flat_tree(boost::move(mx.m_flat_tree))
   {}

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_set<T,Pred,A>& operator=(BOOST_COPY_ASSIGN_REF(flat_set) x)
      {  m_flat_tree = x.m_flat_tree;   return *this;  }

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_set<T,Pred,A>& operator=(BOOST_RV_REF(flat_set) mx)
   {  m_flat_tree = boost::move(mx.m_flat_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
      { return m_flat_tree.key_comp(); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
      { return m_flat_tree.key_comp(); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
      { return m_flat_tree.get_allocator(); }

   const stored_allocator_type &get_stored_allocator() const 
   {  return m_flat_tree.get_stored_allocator(); }

   stored_allocator_type &get_stored_allocator()
   {  return m_flat_tree.get_stored_allocator(); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
      { return m_flat_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
      { return m_flat_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const 
      { return m_flat_tree.cbegin(); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
      { return m_flat_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
      { return m_flat_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const 
      { return m_flat_tree.cend(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
      { return m_flat_tree.rbegin(); } 

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
      { return m_flat_tree.rbegin(); } 

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const 
      { return m_flat_tree.crbegin(); } 

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend()
      { return m_flat_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
      { return m_flat_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const 
      { return m_flat_tree.crend(); }

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   bool empty() const 
      { return m_flat_tree.empty(); }

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type size() const 
      { return m_flat_tree.size(); }

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type max_size() const 
      { return m_flat_tree.max_size(); }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(flat_set& x)
   { m_flat_tree.swap(x.m_flat_tree); }

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
   std::pair<iterator, bool> insert(insert_const_ref_type x) 
   {  return priv_insert(x); }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   std::pair<iterator, bool> insert(T &x)
   { return this->insert(const_cast<const T &>(x)); }

   template<class U>
   std::pair<iterator, bool> insert(const U &u, typename container_detail::enable_if_c<container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   {  return priv_insert(u); }
   #endif

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
   std::pair<iterator,bool> insert(BOOST_RV_REF(value_type) x) 
   {  return m_flat_tree.insert_unique(boost::move(x));  }

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
   iterator insert(const_iterator p, insert_const_ref_type x) 
   {  return priv_insert(p, x); }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   iterator insert(const_iterator position, T &x)
   { return this->insert(position, const_cast<const T &>(x)); }

   template<class U>
   iterator insert(const_iterator position, const U &u, typename container_detail::enable_if_c<container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   {  return priv_insert(position, u); }
   #endif

   //! <b>Effects</b>: Inserts an element move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, BOOST_RV_REF(value_type) x) 
   {  return m_flat_tree.insert_unique(position, boost::move(x)); }

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
      {  m_flat_tree.insert_unique(first, last);  }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object x of type T constructed with
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
   {  return m_flat_tree.emplace_unique(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
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
   {  return m_flat_tree.emplace_hint_unique(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   std::pair<iterator,bool> emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))            \
   {  return m_flat_tree.emplace_unique(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }  \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return m_flat_tree.emplace_hint_unique                                                       \
            (hint BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }               \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

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
   iterator erase(const_iterator position) 
      {  return m_flat_tree.erase(position); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   size_type erase(const key_type& x) 
      {  return m_flat_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: size()*N where N is the distance from first to last.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   iterator erase(const_iterator first, const_iterator last) 
      {  return m_flat_tree.erase(first, last);  }

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear() 
      { m_flat_tree.clear(); }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //    with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit()
      { m_flat_tree.shrink_to_fit(); }

   //! <b>Returns</b>: An iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator find(const key_type& x) 
      { return m_flat_tree.find(x); }

   //! <b>Returns</b>: A const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.s
   const_iterator find(const key_type& x) const 
      { return m_flat_tree.find(x); }

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const 
      {  return m_flat_tree.find(x) == m_flat_tree.end() ? 0 : 1;  }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator lower_bound(const key_type& x) 
      {  return m_flat_tree.lower_bound(x); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
      {  return m_flat_tree.lower_bound(x); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x)
      {  return m_flat_tree.upper_bound(x);    }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
      {  return m_flat_tree.upper_bound(x);    }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator, const_iterator> 
      equal_range(const key_type& x) const 
      {  return m_flat_tree.equal_range(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> 
      equal_range(const key_type& x) 
      {  return m_flat_tree.equal_range(x); }

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type capacity() const           
      { return m_flat_tree.capacity(); }

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //! 
   //! <b>Throws</b>: If memory allocation allocation throws or T's copy constructor throws.
   //!
   //! <b>Note</b>: If capacity() is less than "count", iterators and references to
   //!   to values might be invalidated.
   void reserve(size_type count)       
      { m_flat_tree.reserve(count);   }

   /// @cond
   template <class K1, class C1, class A1>
   friend bool operator== (const flat_set<K1,C1,A1>&, const flat_set<K1,C1,A1>&);

   template <class K1, class C1, class A1>
   friend bool operator< (const flat_set<K1,C1,A1>&, const flat_set<K1,C1,A1>&);

   private:
   std::pair<iterator, bool> priv_insert(const T &x) 
   {  return m_flat_tree.insert_unique(x);  }

   iterator priv_insert(const_iterator p, const T &x) 
   {  return m_flat_tree.insert_unique(p, x); }
   /// @endcond
};

template <class T, class Pred, class A>
inline bool operator==(const flat_set<T,Pred,A>& x, 
                       const flat_set<T,Pred,A>& y) 
   {  return x.m_flat_tree == y.m_flat_tree;  }

template <class T, class Pred, class A>
inline bool operator<(const flat_set<T,Pred,A>& x, 
                      const flat_set<T,Pred,A>& y) 
   {  return x.m_flat_tree < y.m_flat_tree;   }

template <class T, class Pred, class A>
inline bool operator!=(const flat_set<T,Pred,A>& x, 
                       const flat_set<T,Pred,A>& y) 
   {  return !(x == y);   }

template <class T, class Pred, class A>
inline bool operator>(const flat_set<T,Pred,A>& x, 
                      const flat_set<T,Pred,A>& y) 
   {  return y < x; }

template <class T, class Pred, class A>
inline bool operator<=(const flat_set<T,Pred,A>& x, 
                       const flat_set<T,Pred,A>& y) 
   {  return !(y < x); }

template <class T, class Pred, class A>
inline bool operator>=(const flat_set<T,Pred,A>& x, 
                       const flat_set<T,Pred,A>& y) 
   {  return !(x < y);  }

template <class T, class Pred, class A>
inline void swap(flat_set<T,Pred,A>& x, flat_set<T,Pred,A>& y) 
   {  x.swap(y);  }

/// @cond

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class C, class A>
struct has_trivial_destructor_after_move<boost::container::flat_set<T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value &&has_trivial_destructor<C>::value;
};
*/
namespace container {

// Forward declaration of operators < and ==, needed for friend declaration.

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Pred = std::less<T>, class A = std::allocator<T> >
#else
template <class T, class Pred, class A>
#endif
class flat_multiset;

template <class T, class Pred, class A>
inline bool operator==(const flat_multiset<T,Pred,A>& x, 
                       const flat_multiset<T,Pred,A>& y);

template <class T, class Pred, class A>
inline bool operator<(const flat_multiset<T,Pred,A>& x, 
                      const flat_multiset<T,Pred,A>& y);
/// @endcond

//! flat_multiset is a Sorted Associative Container that stores objects of type Key. 
//! flat_multiset is a Simple Associative Container, meaning that its value type, 
//! as well as its key type, is Key.
//! flat_Multiset can store multiple copies of the same key value.
//! 
//! flat_multiset is similar to std::multiset but it's implemented like an ordered vector.
//! This means that inserting a new element into a flat_multiset invalidates
//! previous iterators and references
//!
//! Erasing an element of a flat_multiset invalidates iterators and references 
//! pointing to elements that come after (their keys are equal or bigger) the erased element.
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Pred = std::less<T>, class A = std::allocator<T> >
#else
template <class T, class Pred, class A>
#endif
class flat_multiset 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_multiset)
   typedef container_detail::flat_tree<T, T, container_detail::identity<T>, Pred, A> tree_t;
   tree_t m_flat_tree;  // flat tree representing flat_multiset
   typedef typename container_detail::
      move_const_ref_type<T>::type insert_const_ref_type;
   /// @endcond

   public:
   // typedefs:
   typedef typename tree_t::key_type               key_type;
   typedef typename tree_t::value_type             value_type;
   typedef typename tree_t::pointer                pointer;
   typedef typename tree_t::const_pointer          const_pointer;
   typedef typename tree_t::reference              reference;
   typedef typename tree_t::const_reference        const_reference;
   typedef typename tree_t::key_compare            key_compare;
   typedef typename tree_t::value_compare          value_compare;
   typedef typename tree_t::iterator               iterator;
   typedef typename tree_t::const_iterator         const_iterator;
   typedef typename tree_t::reverse_iterator       reverse_iterator;
   typedef typename tree_t::const_reverse_iterator const_reverse_iterator;
   typedef typename tree_t::size_type              size_type;
   typedef typename tree_t::difference_type        difference_type;
   typedef typename tree_t::allocator_type         allocator_type;
   typedef typename tree_t::stored_allocator_type  stored_allocator_type;

   //! <b>Effects</b>: Defatuls constructs an empty flat_map.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit flat_multiset()
      : m_flat_tree()
   {}

   explicit flat_multiset(const Pred& comp,
                          const allocator_type& a = allocator_type())
      : m_flat_tree(comp, a) {}

   template <class InputIterator>
   flat_multiset(InputIterator first, InputIterator last,
                 const Pred& comp        = Pred(),
                 const allocator_type& a = allocator_type())
      : m_flat_tree(comp, a) 
      { m_flat_tree.insert_equal(first, last); }

   //! <b>Effects</b>: Constructs an empty flat_multiset using the specified comparison object and 
   //! allocator, and inserts elements from the ordered range [first ,last ). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   flat_multiset(ordered_range_t, InputIterator first, InputIterator last,
                 const Pred& comp        = Pred(),
                 const allocator_type& a = allocator_type())
      : m_flat_tree(ordered_range, first, last, comp, a) 
   {}

   flat_multiset(const flat_multiset<T,Pred,A>& x) 
      : m_flat_tree(x.m_flat_tree) {}

   flat_multiset(BOOST_RV_REF(flat_multiset) x) 
      : m_flat_tree(boost::move(x.m_flat_tree))
   {}

   flat_multiset<T,Pred,A>& operator=(BOOST_COPY_ASSIGN_REF(flat_multiset) x) 
      {  m_flat_tree = x.m_flat_tree;   return *this;  }

   flat_multiset<T,Pred,A>& operator=(BOOST_RV_REF(flat_multiset) mx) 
   {  m_flat_tree = boost::move(mx.m_flat_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
      { return m_flat_tree.key_comp(); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
      { return m_flat_tree.key_comp(); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
   { return m_flat_tree.get_allocator(); }

   const stored_allocator_type &get_stored_allocator() const 
   { return m_flat_tree.get_stored_allocator(); }

   stored_allocator_type &get_stored_allocator()
   { return m_flat_tree.get_stored_allocator(); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
      { return m_flat_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
      { return m_flat_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const 
      { return m_flat_tree.cbegin(); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
      { return m_flat_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
      { return m_flat_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const 
      { return m_flat_tree.cend(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
      { return m_flat_tree.rbegin(); } 

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
      { return m_flat_tree.rbegin(); } 

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const 
      { return m_flat_tree.crbegin(); } 

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend()
      { return m_flat_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
      { return m_flat_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const 
      { return m_flat_tree.crend(); }

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   bool empty() const 
      { return m_flat_tree.empty(); }

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type size() const 
      { return m_flat_tree.size(); }

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type max_size() const 
      { return m_flat_tree.max_size(); }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(flat_multiset& x)
   { m_flat_tree.swap(x.m_flat_tree); }

   //! <b>Effects</b>: Inserts x and returns the iterator pointing to the
   //!   newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(insert_const_ref_type x) 
   {  return priv_insert(x); }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   iterator insert(T &x)
   { return this->insert(const_cast<const T &>(x)); }

   template<class U>
   iterator insert(const U &u, typename container_detail::enable_if_c<container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   {  return priv_insert(u); }
   #endif

   //! <b>Effects</b>: Inserts a new value_type move constructed from x
   //!   and returns the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(BOOST_RV_REF(value_type) x) 
   {  return m_flat_tree.insert_equal(boost::move(x));   }

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
   iterator insert(const_iterator p, insert_const_ref_type x) 
   {  return priv_insert(p, x); }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   iterator insert(const_iterator position, T &x)
   { return this->insert(position, const_cast<const T &>(x)); }

   template<class U>
   iterator insert( const_iterator position, const U &u
                  , typename container_detail::enable_if_c<container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   {  return priv_insert(position, u); }
   #endif

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
   iterator insert(const_iterator position, BOOST_RV_REF(value_type) x) 
   {  return m_flat_tree.insert_equal(position, boost::move(x));  }

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
      {  m_flat_tree.insert_equal(first, last);  }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... and returns the iterator pointing to the
   //!   newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class... Args>
   iterator emplace(Args&&... args)
   {  return m_flat_tree.emplace_equal(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
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
   {  return m_flat_tree.emplace_hint_equal(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                            \
   {  return m_flat_tree.emplace_equal(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }   \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return m_flat_tree.emplace_hint_equal                                                        \
            (hint BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }               \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

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
   iterator erase(const_iterator position) 
      {  return m_flat_tree.erase(position); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   size_type erase(const key_type& x) 
      {  return m_flat_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: size()*N where N is the distance from first to last.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   iterator erase(const_iterator first, const_iterator last) 
      {  return m_flat_tree.erase(first, last);  }

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear() 
      { m_flat_tree.clear(); }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //    with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit()
      { m_flat_tree.shrink_to_fit(); }

   //! <b>Returns</b>: An iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator find(const key_type& x) 
      { return m_flat_tree.find(x); }

   //! <b>Returns</b>: A const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.s
   const_iterator find(const key_type& x) const 
      { return m_flat_tree.find(x); }

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const 
      { return m_flat_tree.count(x); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator lower_bound(const key_type& x) 
      {  return m_flat_tree.lower_bound(x); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
      {  return m_flat_tree.lower_bound(x); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x)
      {  return m_flat_tree.upper_bound(x);    }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
      {  return m_flat_tree.upper_bound(x);    }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator, const_iterator> 
      equal_range(const key_type& x) const 
      {  return m_flat_tree.equal_range(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> 
      equal_range(const key_type& x) 
      {  return m_flat_tree.equal_range(x); }

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type capacity() const           
      { return m_flat_tree.capacity(); }

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //! 
   //! <b>Throws</b>: If memory allocation allocation throws or T's copy constructor throws.
   //!
   //! <b>Note</b>: If capacity() is less than "count", iterators and references to
   //!   to values might be invalidated.
   void reserve(size_type count)       
      { m_flat_tree.reserve(count);   }

   /// @cond
   template <class K1, class C1, class A1>
   friend bool operator== (const flat_multiset<K1,C1,A1>&,
                           const flat_multiset<K1,C1,A1>&);
   template <class K1, class C1, class A1>
   friend bool operator< (const flat_multiset<K1,C1,A1>&,
                          const flat_multiset<K1,C1,A1>&);
   private:
   iterator priv_insert(const T &x) 
   {  return m_flat_tree.insert_equal(x);  }

   iterator priv_insert(const_iterator p, const T &x) 
   {  return m_flat_tree.insert_equal(p, x); }
   /// @endcond
};

template <class T, class Pred, class A>
inline bool operator==(const flat_multiset<T,Pred,A>& x, 
                       const flat_multiset<T,Pred,A>& y) 
   {  return x.m_flat_tree == y.m_flat_tree;  }

template <class T, class Pred, class A>
inline bool operator<(const flat_multiset<T,Pred,A>& x, 
                      const flat_multiset<T,Pred,A>& y) 
   {  return x.m_flat_tree < y.m_flat_tree;   }

template <class T, class Pred, class A>
inline bool operator!=(const flat_multiset<T,Pred,A>& x, 
                       const flat_multiset<T,Pred,A>& y) 
   {  return !(x == y);  }

template <class T, class Pred, class A>
inline bool operator>(const flat_multiset<T,Pred,A>& x, 
                      const flat_multiset<T,Pred,A>& y) 
   {  return y < x;  }

template <class T, class Pred, class A>
inline bool operator<=(const flat_multiset<T,Pred,A>& x, 
                       const flat_multiset<T,Pred,A>& y) 
   {  return !(y < x);  }

template <class T, class Pred, class A>
inline bool operator>=(const flat_multiset<T,Pred,A>& x, 
                       const flat_multiset<T,Pred,A>& y) 
{  return !(x < y);  }

template <class T, class Pred, class A>
inline void swap(flat_multiset<T,Pred,A>& x, flat_multiset<T,Pred,A>& y) 
   {  x.swap(y);  }

/// @cond

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class C, class A>
struct has_trivial_destructor_after_move<boost::container::flat_multiset<T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
namespace container {

/// @endcond

}}

#include <boost/container/detail/config_end.hpp>

#endif /* BOOST_CONTAINER_FLAT_SET_HPP */
