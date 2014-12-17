//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_MAP_HPP
#define BOOST_CONTAINER_MAP_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <boost/container/container_fwd.hpp>
#include <utility>
#include <functional>
#include <memory>
#include <stdexcept>
#include <boost/container/detail/tree.hpp>
#include <boost/container/detail/value_init.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/pair.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/move/move.hpp>
#include <boost/move/move_helpers.hpp>
#include <boost/static_assert.hpp>
#include <boost/container/detail/value_init.hpp>


#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
namespace boost {
namespace container {
#else
namespace boost {
namespace container {
#endif

/// @cond
// Forward declarations of operators == and <, needed for friend declarations.
template <class Key, class T, class Pred, class A>
inline bool operator==(const map<Key,T,Pred,A>& x, 
                       const map<Key,T,Pred,A>& y);

template <class Key, class T, class Pred, class A>
inline bool operator<(const map<Key,T,Pred,A>& x, 
                      const map<Key,T,Pred,A>& y);
/// @endcond

//! A map is a kind of associative container that supports unique keys (contains at 
//! most one of each key value) and provides for fast retrieval of values of another 
//! type T based on the keys. The map class supports bidirectional iterators.
//! 
//! A map satisfies all of the requirements of a container and of a reversible 
//! container and of an associative container. For a 
//! map<Key,T> the key_type is Key and the value_type is std::pair<const Key,T>.
//!
//! Pred is the ordering function for Keys (e.g. <i>std::less<Key></i>).
//!
//! A is the allocator to allocate the value_types
//! (e.g. <i>allocator< std::pair<const Key, T> > </i>).
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< const Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class map 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(map)
   typedef container_detail::rbtree<Key, 
                           std::pair<const Key, T>, 
                           container_detail::select1st< std::pair<const Key, T> >, 
                           Pred, 
                           A> tree_t;
   tree_t m_tree;  // red-black tree representing map

   /// @endcond

   public:

   // typedefs:
   typedef typename tree_t::key_type               key_type;
   typedef typename tree_t::value_type             value_type;
   typedef typename tree_t::pointer                pointer;
   typedef typename tree_t::const_pointer          const_pointer;
   typedef typename tree_t::reference              reference;
   typedef typename tree_t::const_reference        const_reference;
   typedef T                                       mapped_type;
   typedef Pred                                    key_compare;
   typedef typename tree_t::iterator               iterator;
   typedef typename tree_t::const_iterator         const_iterator;
   typedef typename tree_t::reverse_iterator       reverse_iterator;
   typedef typename tree_t::const_reverse_iterator const_reverse_iterator;
   typedef typename tree_t::size_type              size_type;
   typedef typename tree_t::difference_type        difference_type;
   typedef typename tree_t::allocator_type         allocator_type;
   typedef typename tree_t::stored_allocator_type  stored_allocator_type;
   typedef std::pair<key_type, mapped_type>        nonconst_value_type;
   typedef container_detail::pair
      <key_type, mapped_type>                      nonconst_impl_value_type;

   /// @cond
   class value_compare_impl
      :  public Pred,
         public std::binary_function<value_type, value_type, bool> 
   {
      friend class map<Key,T,Pred,A>;
    protected :
      value_compare_impl(const Pred &c) : Pred(c) {}
    public:
      bool operator()(const value_type& x, const value_type& y) const {
         return Pred::operator()(x.first, y.first);
      }
   };
   /// @endcond
   typedef value_compare_impl             value_compare;

   //! <b>Effects</b>: Default constructs an empty map.
   //! 
   //! <b>Complexity</b>: Constant.
   map()
      : m_tree()
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty map using the specified comparison object 
   //! and allocator.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit map(const Pred& comp,
                const allocator_type& a = allocator_type())
      : m_tree(comp, a)
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty map using the specified comparison object and 
   //! allocator, and inserts elements from the range [first ,last ).
   //! 
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using 
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   map(InputIterator first, InputIterator last, const Pred& comp = Pred(),
         const allocator_type& a = allocator_type())
      : m_tree(first, last, comp, a, true) 
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty map using the specified comparison object and 
   //! allocator, and inserts elements from the ordered unique range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate and must be
   //! unique values.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   map( ordered_unique_range_t, InputIterator first, InputIterator last
      , const Pred& comp = Pred(), const allocator_type& a = allocator_type())
      : m_tree(ordered_range, first, last, comp, a) 
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Copy constructs a map.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   map(const map<Key,T,Pred,A>& x) 
      : m_tree(x.m_tree)
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Move constructs a map. Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   map(BOOST_RV_REF(map) x) 
      : m_tree(boost::move(x.m_tree))
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   map& operator=(BOOST_COPY_ASSIGN_REF(map) x)
   {  m_tree = x.m_tree;   return *this;  }

   //! <b>Effects</b>: this->swap(x.get()).
   //! 
   //! <b>Complexity</b>: Constant.
   map& operator=(BOOST_RV_REF(map) x)
   {  m_tree = boost::move(x.m_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
   { return m_tree.key_comp(); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
   { return value_compare(m_tree.key_comp()); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
   { return m_tree.get_allocator(); }

   const stored_allocator_type &get_stored_allocator() const 
   { return m_tree.get_stored_allocator(); }

   stored_allocator_type &get_stored_allocator()
   { return m_tree.get_stored_allocator(); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
   { return m_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
   { return m_tree.begin(); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
   { return m_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
   { return m_tree.end(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
   { return m_tree.rbegin(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
   { return m_tree.rbegin(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() 
   { return m_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
   { return m_tree.rend(); }

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   bool empty() const 
   { return m_tree.empty(); }

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type size() const 
   { return m_tree.size(); }

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type max_size() const 
   { return m_tree.max_size(); }

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! Effects: If there is no key equivalent to x in the map, inserts 
   //! value_type(x, T()) into the map.
   //! 
   //! Returns: A reference to the mapped_type corresponding to x in *this.
   //! 
   //! Complexity: Logarithmic.
   mapped_type& operator[](const key_type &k);

   //! Effects: If there is no key equivalent to x in the map, inserts 
   //! value_type(boost::move(x), T()) into the map (the key is move-constructed)
   //! 
   //! Returns: A reference to the mapped_type corresponding to x in *this.
   //! 
   //! Complexity: Logarithmic.
   mapped_type& operator[](key_type &&k);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH( operator[] , key_type, mapped_type&, priv_subscript)
   #endif

   //! Returns: A reference to the element whose key is equivalent to x.
   //! Throws: An exception object of type out_of_range if no such element is present.
   //! Complexity: logarithmic.
   T& at(const key_type& k)
   {
      iterator i = this->find(k);
      if(i == this->end()){
         throw std::out_of_range("key not found");
      }
      return i->second;
   }

   //! Returns: A reference to the element whose key is equivalent to x.
   //! Throws: An exception object of type out_of_range if no such element is present.
   //! Complexity: logarithmic.
   const T& at(const key_type& k) const
   {
      const_iterator i = this->find(k);
      if(i == this->end()){
         throw std::out_of_range("key not found");
      }
      return i->second;
   }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(map& x)
   { m_tree.swap(x.m_tree); }

   //! <b>Effects</b>: Inserts x if and only if there is no element in the container 
   //!   with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator,bool> insert(const value_type& x) 
   { return m_tree.insert_unique(x); }

   //! <b>Effects</b>: Inserts a new value_type created from the pair if and only if 
   //! there is no element in the container  with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator,bool> insert(const nonconst_value_type& x) 
   { return m_tree.insert_unique(x); }

   //! <b>Effects</b>: Inserts a new value_type move constructed from the pair if and
   //! only if there is no element in the container with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator,bool> insert(BOOST_RV_REF(nonconst_value_type) x) 
   { return m_tree.insert_unique(boost::move(x)); }

   //! <b>Effects</b>: Inserts a new value_type move constructed from the pair if and
   //! only if there is no element in the container with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator,bool> insert(BOOST_RV_REF(nonconst_impl_value_type) x) 
   { return m_tree.insert_unique(boost::move(x)); }

   //! <b>Effects</b>: Move constructs a new value from x if and only if there is 
   //!   no element in the container with key equivalent to the key of x.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   std::pair<iterator,bool> insert(BOOST_RV_REF(value_type) x) 
   { return m_tree.insert_unique(boost::move(x)); }

   //! <b>Effects</b>: Inserts a copy of x in the container if and only if there is 
   //!   no element in the container with key equivalent to the key of x.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, const value_type& x)
   { return m_tree.insert_unique(position, x); }

   //! <b>Effects</b>: Move constructs a new value from x if and only if there is 
   //!   no element in the container with key equivalent to the key of x.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, BOOST_RV_REF(nonconst_value_type) x)
   { return m_tree.insert_unique(position, boost::move(x)); }

   //! <b>Effects</b>: Move constructs a new value from x if and only if there is 
   //!   no element in the container with key equivalent to the key of x.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, BOOST_RV_REF(nonconst_impl_value_type) x)
   { return m_tree.insert_unique(position, boost::move(x)); }

   //! <b>Effects</b>: Inserts a copy of x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(iterator position, const nonconst_value_type& x)
   { return m_tree.insert_unique(position, x); }

   //! <b>Effects</b>: Inserts an element move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(iterator position, BOOST_RV_REF(value_type) x)
   { return m_tree.insert_unique(position, boost::move(x)); }

   //! <b>Requires</b>: first, last are not iterators into *this.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) if and only 
   //!   if there is no element with key equivalent to the key of that element.
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last) 
   {  m_tree.insert_unique(first, last);  }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object x of type T constructed with
   //!   std::forward<Args>(args)... in the container if and only if there is 
   //!   no element in the container with an equivalent key.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: The bool component of the returned pair is true if and only 
   //!   if the insertion takes place, and the iterator component of the pair
   //!   points to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   template <class... Args>
   std::pair<iterator,bool> emplace(Args&&... args)
   {  return m_tree.emplace_unique(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the container if and only if there is 
   //!   no element in the container with an equivalent key.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {  return m_tree.emplace_hint_unique(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   std::pair<iterator,bool> emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))            \
   {  return m_tree.emplace_unique(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }       \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return m_tree.emplace_hint_unique(hint                                                       \
                               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));}   \
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
   //! <b>Complexity</b>: Amortized constant time
   iterator erase(const_iterator position) 
   { return m_tree.erase(position); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: log(size()) + count(k)
   size_type erase(const key_type& x) 
   { return m_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: log(size())+N where N is the distance from first to last.
   iterator erase(const_iterator first, const_iterator last)
   { return m_tree.erase(first, last); }

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear() 
   { m_tree.clear(); }

   //! <b>Returns</b>: An iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator find(const key_type& x) 
   { return m_tree.find(x); }

   //! <b>Returns</b>: A const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   const_iterator find(const key_type& x) const 
   { return m_tree.find(x); }

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const 
   {  return m_tree.find(x) == m_tree.end() ? 0 : 1;  }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator lower_bound(const key_type& x) 
   {  return m_tree.lower_bound(x); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
   {  return m_tree.lower_bound(x); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x) 
   {  return m_tree.upper_bound(x); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
   {  return m_tree.upper_bound(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x) 
   {  return m_tree.equal_range(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator,const_iterator> equal_range(const key_type& x) const 
   {  return m_tree.equal_range(x); }

   /// @cond
   template <class K1, class T1, class C1, class A1>
   friend bool operator== (const map<K1, T1, C1, A1>&,
                           const map<K1, T1, C1, A1>&);
   template <class K1, class T1, class C1, class A1>
   friend bool operator< (const map<K1, T1, C1, A1>&,
                          const map<K1, T1, C1, A1>&);
   private:
   mapped_type& priv_subscript(const key_type &k)
   {
      //we can optimize this
      iterator i = lower_bound(k);
      // i->first is greater than or equivalent to k.
      if (i == end() || key_comp()(k, (*i).first)){
         container_detail::value_init<mapped_type> m;
         nonconst_impl_value_type val(k, boost::move(m.m_t));
         i = insert(i, boost::move(val));
      }
      return (*i).second;
   }

   mapped_type& priv_subscript(BOOST_RV_REF(key_type) mk) 
   {
      key_type &k = mk;
      //we can optimize this
      iterator i = lower_bound(k);
      // i->first is greater than or equivalent to k.
      if (i == end() || key_comp()(k, (*i).first)){
         container_detail::value_init<mapped_type> m;
         nonconst_impl_value_type val(boost::move(k), boost::move(m.m_t));
         i = insert(i, boost::move(val));
      }
      return (*i).second;
   }

   /// @endcond
};

template <class Key, class T, class Pred, class A>
inline bool operator==(const map<Key,T,Pred,A>& x, 
                       const map<Key,T,Pred,A>& y) 
   {  return x.m_tree == y.m_tree;  }

template <class Key, class T, class Pred, class A>
inline bool operator<(const map<Key,T,Pred,A>& x, 
                      const map<Key,T,Pred,A>& y) 
   {  return x.m_tree < y.m_tree;   }

template <class Key, class T, class Pred, class A>
inline bool operator!=(const map<Key,T,Pred,A>& x, 
                       const map<Key,T,Pred,A>& y) 
   {  return !(x == y); }

template <class Key, class T, class Pred, class A>
inline bool operator>(const map<Key,T,Pred,A>& x, 
                      const map<Key,T,Pred,A>& y) 
   {  return y < x;  }

template <class Key, class T, class Pred, class A>
inline bool operator<=(const map<Key,T,Pred,A>& x, 
                       const map<Key,T,Pred,A>& y) 
   {  return !(y < x);  }

template <class Key, class T, class Pred, class A>
inline bool operator>=(const map<Key,T,Pred,A>& x, 
                       const map<Key,T,Pred,A>& y) 
   {  return !(x < y);  }

template <class Key, class T, class Pred, class A>
inline void swap(map<Key,T,Pred,A>& x, map<Key,T,Pred,A>& y) 
   {  x.swap(y);  }

/// @cond

// Forward declaration of operators < and ==, needed for friend declaration.

template <class Key, class T, class Pred, class A>
inline bool operator==(const multimap<Key,T,Pred,A>& x, 
                       const multimap<Key,T,Pred,A>& y);

template <class Key, class T, class Pred, class A>
inline bool operator<(const multimap<Key,T,Pred,A>& x, 
                      const multimap<Key,T,Pred,A>& y);

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class K, class T, class C, class A>
struct has_trivial_destructor_after_move<boost::container::map<K, T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
namespace container {

/// @endcond

//! A multimap is a kind of associative container that supports equivalent keys 
//! (possibly containing multiple copies of the same key value) and provides for 
//! fast retrieval of values of another type T based on the keys. The multimap class
//! supports bidirectional iterators.
//! 
//! A multimap satisfies all of the requirements of a container and of a reversible 
//! container and of an associative container. For a 
//! map<Key,T> the key_type is Key and the value_type is std::pair<const Key,T>. 
//!
//! Pred is the ordering function for Keys (e.g. <i>std::less<Key></i>).
//!
//! A is the allocator to allocate the value_types
//!(e.g. <i>allocator< std::pair<<b>const</b> Key, T> ></i>).
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< const Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class multimap 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(multimap)
   typedef container_detail::rbtree<Key, 
                           std::pair<const Key, T>, 
                           container_detail::select1st< std::pair<const Key, T> >, 
                           Pred, 
                           A> tree_t;
   tree_t m_tree;  // red-black tree representing map
   typedef typename container_detail::
      move_const_ref_type<Key>::type insert_key_const_ref_type;
   /// @endcond

   public:

   // typedefs:
   typedef typename tree_t::key_type               key_type;
   typedef typename tree_t::value_type             value_type;
   typedef typename tree_t::pointer                pointer;
   typedef typename tree_t::const_pointer          const_pointer;
   typedef typename tree_t::reference              reference;
   typedef typename tree_t::const_reference        const_reference;
   typedef T                                       mapped_type;
   typedef Pred                                    key_compare;
   typedef typename tree_t::iterator               iterator;
   typedef typename tree_t::const_iterator         const_iterator;
   typedef typename tree_t::reverse_iterator       reverse_iterator;
   typedef typename tree_t::const_reverse_iterator const_reverse_iterator;
   typedef typename tree_t::size_type              size_type;
   typedef typename tree_t::difference_type        difference_type;
   typedef typename tree_t::allocator_type         allocator_type;
   typedef typename tree_t::stored_allocator_type  stored_allocator_type;
   typedef std::pair<key_type, mapped_type>        nonconst_value_type;
   typedef container_detail::pair
      <key_type, mapped_type>                      nonconst_impl_value_type;

   /// @cond
   class value_compare_impl
      :  public Pred,
         public std::binary_function<value_type, value_type, bool> 
   {
      friend class multimap<Key,T,Pred,A>;
    protected :
      value_compare_impl(const Pred &c) : Pred(c) {}
    public:
      bool operator()(const value_type& x, const value_type& y) const {
         return Pred::operator()(x.first, y.first);
      }
   };
   /// @endcond
   typedef value_compare_impl                      value_compare;

   //! <b>Effects</b>: Default constructs an empty multimap.
   //! 
   //! <b>Complexity</b>: Constant.
   multimap()
      : m_tree()
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty multimap using the specified comparison
   //!   object and allocator.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit multimap(const Pred& comp, const allocator_type& a = allocator_type())
      : m_tree(comp, a)
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty multimap using the specified comparison object
   //!   and allocator, and inserts elements from the range [first ,last ).
   //! 
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using 
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   multimap(InputIterator first, InputIterator last,
            const Pred& comp = Pred(),
            const allocator_type& a = allocator_type())
      : m_tree(first, last, comp, a, false) 
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Constructs an empty multimap using the specified comparison object and 
   //! allocator, and inserts elements from the ordered range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   multimap(ordered_range_t ordered_range, InputIterator first, InputIterator last, const Pred& comp = Pred(),
         const allocator_type& a = allocator_type())
      : m_tree(ordered_range, first, last, comp, a) 
   {}


   //! <b>Effects</b>: Copy constructs a multimap.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   multimap(const multimap<Key,T,Pred,A>& x) 
      : m_tree(x.m_tree)
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Move constructs a multimap. Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   multimap(BOOST_RV_REF(multimap) x) 
      : m_tree(boost::move(x.m_tree))
   {
      //Allocator type must be std::pair<CONST Key, T>
      BOOST_STATIC_ASSERT((container_detail::is_same<std::pair<const Key, T>, typename     A::value_type>::value));
   }

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   multimap& operator=(BOOST_COPY_ASSIGN_REF(multimap) x) 
   {  m_tree = x.m_tree;   return *this;  }

   //! <b>Effects</b>: this->swap(x.get()).
   //! 
   //! <b>Complexity</b>: Constant.
   multimap& operator=(BOOST_RV_REF(multimap) x) 
   {  m_tree = boost::move(x.m_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
   { return m_tree.key_comp(); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
   { return value_compare(m_tree.key_comp()); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
   { return m_tree.get_allocator(); }

   const stored_allocator_type &get_stored_allocator() const 
   { return m_tree.get_stored_allocator(); }

   stored_allocator_type &get_stored_allocator()
   { return m_tree.get_stored_allocator(); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
   { return m_tree.begin(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
   { return m_tree.begin(); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
   { return m_tree.end(); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
   { return m_tree.end(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
   { return m_tree.rbegin(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
   { return m_tree.rbegin(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() 
   { return m_tree.rend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
   { return m_tree.rend(); }

   //! <b>Effects</b>: Returns true if the container contains no elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   bool empty() const 
   { return m_tree.empty(); }

   //! <b>Effects</b>: Returns the number of the elements contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type size() const 
   { return m_tree.size(); }

   //! <b>Effects</b>: Returns the largest possible size of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type max_size() const 
   { return m_tree.max_size(); }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(multimap& x)
   { m_tree.swap(x.m_tree); }

   //! <b>Effects</b>: Inserts x and returns the iterator pointing to the
   //!   newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(const value_type& x) 
   { return m_tree.insert_equal(x); }

   //! <b>Effects</b>: Inserts a new value constructed from x and returns 
   //!   the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(const nonconst_value_type& x) 
   { return m_tree.insert_equal(x); }

   //! <b>Effects</b>: Inserts a new value move-constructed from x and returns 
   //!   the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(BOOST_RV_REF(nonconst_value_type) x) 
   { return m_tree.insert_equal(boost::move(x)); }

   //! <b>Effects</b>: Inserts a new value move-constructed from x and returns 
   //!   the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator insert(BOOST_RV_REF(nonconst_impl_value_type) x) 
   { return m_tree.insert_equal(boost::move(x)); }

   //! <b>Effects</b>: Inserts a copy of x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, const value_type& x)
   { return m_tree.insert_equal(position, x); }

   //! <b>Effects</b>: Inserts a new value constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, const nonconst_value_type& x)
   { return m_tree.insert_equal(position, x); }

   //! <b>Effects</b>: Inserts a new value move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, BOOST_RV_REF(nonconst_value_type) x)
   { return m_tree.insert_equal(position, boost::move(x)); }

   //! <b>Effects</b>: Inserts a new value move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   iterator insert(iterator position, BOOST_RV_REF(nonconst_impl_value_type) x)
   { return m_tree.insert_equal(position, boost::move(x)); }

   //! <b>Requires</b>: first, last are not iterators into *this.
   //!
   //! <b>Effects</b>: inserts each element from the range [first,last) .
   //!
   //! <b>Complexity</b>: At most N log(size()+N) (N is the distance from first to last)
   template <class InputIterator>
   void insert(InputIterator first, InputIterator last) 
   {  m_tree.insert_equal(first, last); }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   template <class... Args>
   iterator emplace(Args&&... args)
   {  return m_tree.emplace_equal(boost::forward<Args>(args)...); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but amortized constant if t
   //!   is inserted right before p.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {  return m_tree.emplace_hint_equal(hint, boost::forward<Args>(args)...); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                            \
   {  return m_tree.emplace_equal(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)); }        \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return m_tree.emplace_hint_equal(hint                                                        \
                               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));}   \
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
   //! <b>Complexity</b>: Amortized constant time
   iterator erase(const_iterator position) 
   { return m_tree.erase(position); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: log(size()) + count(k)
   size_type erase(const key_type& x) 
   { return m_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: log(size())+N where N is the distance from first to last.
   iterator erase(const_iterator first, const_iterator last)
   { return m_tree.erase(first, last); }

   //! <b>Effects</b>: erase(a.begin(),a.end()).
   //!
   //! <b>Postcondition</b>: size() == 0.
   //!
   //! <b>Complexity</b>: linear in size().
   void clear() 
   { m_tree.clear(); }

   //! <b>Returns</b>: An iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   iterator find(const key_type& x) 
   { return m_tree.find(x); }

   //! <b>Returns</b>: A const iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   const_iterator find(const key_type& x) const 
   { return m_tree.find(x); }

   //! <b>Returns</b>: The number of elements with key equivalent to x.
   //!
   //! <b>Complexity</b>: log(size())+count(k)
   size_type count(const key_type& x) const 
   { return m_tree.count(x); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator lower_bound(const key_type& x) 
   {return m_tree.lower_bound(x); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
   {  return m_tree.lower_bound(x);  }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x) 
   {  return m_tree.upper_bound(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x) 
   {  return m_tree.equal_range(x);   }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
   {  return m_tree.upper_bound(x); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator,const_iterator> 
      equal_range(const key_type& x) const 
   {  return m_tree.equal_range(x);   }

   /// @cond
   template <class K1, class T1, class C1, class A1>
   friend bool operator== (const multimap<K1, T1, C1, A1>& x,
                           const multimap<K1, T1, C1, A1>& y);

   template <class K1, class T1, class C1, class A1>
   friend bool operator< (const multimap<K1, T1, C1, A1>& x,
                          const multimap<K1, T1, C1, A1>& y);
   /// @endcond
};

template <class Key, class T, class Pred, class A>
inline bool operator==(const multimap<Key,T,Pred,A>& x, 
                       const multimap<Key,T,Pred,A>& y) 
{  return x.m_tree == y.m_tree;  }

template <class Key, class T, class Pred, class A>
inline bool operator<(const multimap<Key,T,Pred,A>& x, 
                      const multimap<Key,T,Pred,A>& y) 
{  return x.m_tree < y.m_tree;   }

template <class Key, class T, class Pred, class A>
inline bool operator!=(const multimap<Key,T,Pred,A>& x, 
                       const multimap<Key,T,Pred,A>& y) 
{  return !(x == y);  }

template <class Key, class T, class Pred, class A>
inline bool operator>(const multimap<Key,T,Pred,A>& x, 
                      const multimap<Key,T,Pred,A>& y) 
{  return y < x;  }

template <class Key, class T, class Pred, class A>
inline bool operator<=(const multimap<Key,T,Pred,A>& x, 
                       const multimap<Key,T,Pred,A>& y) 
{  return !(y < x);  }

template <class Key, class T, class Pred, class A>
inline bool operator>=(const multimap<Key,T,Pred,A>& x, 
                       const multimap<Key,T,Pred,A>& y) 
{  return !(x < y);  }

template <class Key, class T, class Pred, class A>
inline void swap(multimap<Key,T,Pred,A>& x, multimap<Key,T,Pred,A>& y) 
{  x.swap(y);  }

/// @cond

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class K, class T, class C, class A>
struct has_trivial_destructor_after_move<boost::container::multimap<K, T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
namespace container {

/// @endcond

}}

#include <boost/container/detail/config_end.hpp>

#endif /* BOOST_CONTAINER_MAP_HPP */

