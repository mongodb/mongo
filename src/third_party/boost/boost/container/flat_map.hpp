//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_FLAT_MAP_HPP
#define BOOST_CONTAINER_FLAT_MAP_HPP

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
#include <boost/container/detail/flat_tree.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/allocator/allocator_traits.hpp>
#include <boost/move/move.hpp>

#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
namespace boost {
namespace container {
#else
namespace boost {
namespace container {
#endif

/// @cond
// Forward declarations of operators == and <, needed for friend declarations.
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class flat_map;

template <class Key, class T, class Pred, class A>
inline bool operator==(const flat_map<Key,T,Pred,A>& x, 
                       const flat_map<Key,T,Pred,A>& y);

template <class Key, class T, class Pred, class A>
inline bool operator<(const flat_map<Key,T,Pred,A>& x, 
                      const flat_map<Key,T,Pred,A>& y);

namespace container_detail{

template<class D, class S>
static D &force(const S &s)
{  return *const_cast<D*>((reinterpret_cast<const D*>(&s))); }

template<class D, class S>
static D force_copy(S s)
{
   D *vp = reinterpret_cast<D *>(&s);
   return D(*vp);
}

}  //namespace container_detail{


/// @endcond

//! A flat_map is a kind of associative container that supports unique keys (contains at
//! most one of each key value) and provides for fast retrieval of values of another 
//! type T based on the keys. The flat_map class supports random-access iterators.
//! 
//! A flat_map satisfies all of the requirements of a container and of a reversible 
//! container and of an associative container. A flat_map also provides 
//! most operations described for unique keys. For a 
//! flat_map<Key,T> the key_type is Key and the value_type is std::pair<Key,T>
//! (unlike std::map<Key, T> which value_type is std::pair<<b>const</b> Key, T>).
//!
//! Pred is the ordering function for Keys (e.g. <i>std::less<Key></i>).
//!
//! A is the allocator to allocate the value_types
//! (e.g. <i>allocator< std::pair<Key, T> ></i>).
//! 
//! flat_map is similar to std::map but it's implemented like an ordered vector.
//! This means that inserting a new element into a flat_map invalidates
//! previous iterators and references
//!
//! Erasing an element of a flat_map invalidates iterators and references 
//! pointing to elements that come after (their keys are bigger) the erased element.
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class flat_map 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_map)
   //This is the tree that we should store if pair was movable
   typedef container_detail::flat_tree<Key, 
                           std::pair<Key, T>, 
                           container_detail::select1st< std::pair<Key, T> >, 
                           Pred, 
                           A> tree_t;

   //This is the real tree stored here. It's based on a movable pair
   typedef container_detail::flat_tree<Key, 
                           container_detail::pair<Key, T>, 
                           container_detail::select1st<container_detail::pair<Key, T> >, 
                           Pred, 
                           typename allocator_traits<A>::template portable_rebind_alloc
                              <container_detail::pair<Key, T> >::type> impl_tree_t;
   impl_tree_t m_flat_tree;  // flat tree representing flat_map

   typedef typename impl_tree_t::value_type              impl_value_type;
   typedef typename impl_tree_t::pointer                 impl_pointer;
   typedef typename impl_tree_t::const_pointer           impl_const_pointer;
   typedef typename impl_tree_t::reference               impl_reference;
   typedef typename impl_tree_t::const_reference         impl_const_reference;
   typedef typename impl_tree_t::value_compare           impl_value_compare;
   typedef typename impl_tree_t::iterator                impl_iterator;
   typedef typename impl_tree_t::const_iterator          impl_const_iterator;
   typedef typename impl_tree_t::reverse_iterator        impl_reverse_iterator;
   typedef typename impl_tree_t::const_reverse_iterator  impl_const_reverse_iterator;
   typedef typename impl_tree_t::allocator_type          impl_allocator_type;
   typedef allocator_traits<A>                           allocator_traits_type;



   /// @endcond

   public:

   // typedefs:
   typedef Key                                              key_type;
   typedef T                                                mapped_type;
   typedef typename std::pair<key_type, mapped_type>        value_type;
   typedef typename allocator_traits_type::pointer          pointer;
   typedef typename allocator_traits_type::const_pointer    const_pointer;
   typedef typename allocator_traits_type::reference        reference;
   typedef typename allocator_traits_type::const_reference  const_reference;
   typedef typename impl_tree_t::size_type                  size_type;
   typedef typename impl_tree_t::difference_type            difference_type;

   typedef container_detail::flat_tree_value_compare
      < Pred
      , container_detail::select1st< std::pair<Key, T> >
      , std::pair<Key, T> >                                 value_compare;
   typedef Pred                                             key_compare;
   typedef typename container_detail::
      get_flat_tree_iterators<pointer>::iterator            iterator;
   typedef typename container_detail::
      get_flat_tree_iterators<pointer>::const_iterator      const_iterator;
   typedef typename container_detail::
      get_flat_tree_iterators
         <pointer>::reverse_iterator                        reverse_iterator;
   typedef typename container_detail::
      get_flat_tree_iterators
         <pointer>::const_reverse_iterator                  const_reverse_iterator;
   typedef A                                                allocator_type;
   typedef A                                                stored_allocator_type;

   public:
   //! <b>Effects</b>: Default constructs an empty flat_map.
   //! 
   //! <b>Complexity</b>: Constant.
   flat_map() 
      : m_flat_tree() {}

   //! <b>Effects</b>: Constructs an empty flat_map using the specified
   //! comparison object and allocator.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit flat_map(const Pred& comp, const allocator_type& a = allocator_type()) 
      : m_flat_tree(comp, container_detail::force<impl_allocator_type>(a)) {}

   //! <b>Effects</b>: Constructs an empty flat_map using the specified comparison object and 
   //! allocator, and inserts elements from the range [first ,last ).
   //! 
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using 
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   flat_map(InputIterator first, InputIterator last, const Pred& comp = Pred(),
         const allocator_type& a = allocator_type())
      : m_flat_tree(comp, container_detail::force<impl_allocator_type>(a)) 
      { m_flat_tree.insert_unique(first, last); }

   //! <b>Effects</b>: Constructs an empty flat_map using the specified comparison object and 
   //! allocator, and inserts elements from the ordered unique range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate and must be
   //! unique values.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   flat_map( ordered_unique_range_t, InputIterator first, InputIterator last
           , const Pred& comp = Pred(), const allocator_type& a = allocator_type())
      : m_flat_tree(ordered_range, first, last, comp, a) 
   {}

   //! <b>Effects</b>: Copy constructs a flat_map.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_map(const flat_map<Key,T,Pred,A>& x) 
      : m_flat_tree(x.m_flat_tree) {}

   //! <b>Effects</b>: Move constructs a flat_map.
   //!   Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   flat_map(BOOST_RV_REF(flat_map) x) 
      : m_flat_tree(boost::move(x.m_flat_tree))
   {}

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_map<Key,T,Pred,A>& operator=(BOOST_COPY_ASSIGN_REF(flat_map) x)
   {  m_flat_tree = x.m_flat_tree;   return *this;  }

   //! <b>Effects</b>: Move constructs a flat_map.
   //!   Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   flat_map<Key,T,Pred,A>& operator=(BOOST_RV_REF(flat_map) mx)
   {  m_flat_tree = boost::move(mx.m_flat_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
      { return container_detail::force<key_compare>(m_flat_tree.key_comp()); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
      { return value_compare(container_detail::force<key_compare>(m_flat_tree.key_comp())); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
      { return container_detail::force<allocator_type>(m_flat_tree.get_allocator()); }

   const stored_allocator_type &get_stored_allocator() const 
      { return container_detail::force<stored_allocator_type>(m_flat_tree.get_stored_allocator()); }

   stored_allocator_type &get_stored_allocator()
      { return container_detail::force<stored_allocator_type>(m_flat_tree.get_stored_allocator()); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
      { return container_detail::force_copy<iterator>(m_flat_tree.begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
      { return container_detail::force<const_iterator>(m_flat_tree.begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const 
      { return container_detail::force<const_iterator>(m_flat_tree.cbegin()); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
      { return container_detail::force_copy<iterator>(m_flat_tree.end()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
      { return container_detail::force<const_iterator>(m_flat_tree.end()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const 
      { return container_detail::force<const_iterator>(m_flat_tree.cend()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
      { return container_detail::force<reverse_iterator>(m_flat_tree.rbegin()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.rbegin()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.crbegin()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() 
      { return container_detail::force<reverse_iterator>(m_flat_tree.rend()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.rend()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.crend()); }

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

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! Effects: If there is no key equivalent to x in the flat_map, inserts 
   //!   value_type(x, T()) into the flat_map.
   //! 
   //! Returns: A reference to the mapped_type corresponding to x in *this.
   //! 
   //! Complexity: Logarithmic.
   mapped_type &operator[](const key_type& k);

   //! Effects: If there is no key equivalent to x in the flat_map, inserts 
   //! value_type(move(x), T()) into the flat_map (the key is move-constructed)
   //! 
   //! Returns: A reference to the mapped_type corresponding to x in *this.
   //! 
   //! Complexity: Logarithmic.
   mapped_type &operator[](key_type &&k) ;

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
   void swap(flat_map& x)
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
   std::pair<iterator,bool> insert(const value_type& x) 
      { return container_detail::force<std::pair<iterator,bool> >(
         m_flat_tree.insert_unique(container_detail::force<impl_value_type>(x))); }

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
   {  return container_detail::force<std::pair<iterator,bool> >(
      m_flat_tree.insert_unique(boost::move(container_detail::force<impl_value_type>(x)))); }

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
   std::pair<iterator,bool> insert(BOOST_RV_REF(impl_value_type) x) 
   {
      return container_detail::force<std::pair<iterator,bool> >
      (m_flat_tree.insert_unique(boost::move(x)));
   }

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
   iterator insert(const_iterator position, const value_type& x)
      { return container_detail::force_copy<iterator>(
         m_flat_tree.insert_unique(container_detail::force<impl_const_iterator>(position), container_detail::force<impl_value_type>(x))); }

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
      { return container_detail::force_copy<iterator>
         (m_flat_tree.insert_unique(container_detail::force<impl_const_iterator>(position), boost::move(container_detail::force<impl_value_type>(x)))); }

   //! <b>Effects</b>: Inserts an element move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant if x is inserted
   //!   right before p) plus insertion linear to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, BOOST_RV_REF(impl_value_type) x)
   {
      return container_detail::force_copy<iterator>(
         m_flat_tree.insert_unique(container_detail::force<impl_const_iterator>(position), boost::move(x)));
   }

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
   {  return container_detail::force_copy< std::pair<iterator, bool> >(m_flat_tree.emplace_unique(boost::forward<Args>(args)...)); }

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
   {  return container_detail::force_copy<iterator>
      (m_flat_tree.emplace_hint_unique(container_detail::force<impl_const_iterator>(hint), boost::forward<Args>(args)...)); }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   std::pair<iterator,bool> emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))            \
   {  return container_detail::force_copy< std::pair<iterator, bool> >                             \
         (m_flat_tree.emplace_unique(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))); }    \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return container_detail::force_copy<iterator>(m_flat_tree.emplace_hint_unique                \
            (container_detail::force<impl_const_iterator>(hint)                                    \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))); }                 \
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
      { return container_detail::force_copy<iterator>(m_flat_tree.erase(container_detail::force<impl_const_iterator>(position))); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   size_type erase(const key_type& x) 
      { return m_flat_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: size()*N where N is the distance from first to last.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   iterator erase(const_iterator first, const_iterator last)
      { return container_detail::force_copy<iterator>
         (m_flat_tree.erase(container_detail::force<impl_const_iterator>(first), container_detail::force<impl_const_iterator>(last))); }

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
      { return container_detail::force_copy<iterator>(m_flat_tree.find(x)); }

   //! <b>Returns</b>: A const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.s
   const_iterator find(const key_type& x) const 
      { return container_detail::force<const_iterator>(m_flat_tree.find(x)); }

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
      {  return container_detail::force_copy<iterator>(m_flat_tree.lower_bound(x)); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
      {  return container_detail::force<const_iterator>(m_flat_tree.lower_bound(x)); }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x) 
      {  return container_detail::force_copy<iterator>(m_flat_tree.upper_bound(x)); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key not
   //!   less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
      {  return container_detail::force<const_iterator>(m_flat_tree.upper_bound(x)); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x) 
      {  return container_detail::force<std::pair<iterator,iterator> >(m_flat_tree.equal_range(x)); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator,const_iterator> equal_range(const key_type& x) const 
      {  return container_detail::force<std::pair<const_iterator,const_iterator> >(m_flat_tree.equal_range(x)); }

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
   template <class K1, class T1, class C1, class A1>
   friend bool operator== (const flat_map<K1, T1, C1, A1>&,
                           const flat_map<K1, T1, C1, A1>&);
   template <class K1, class T1, class C1, class A1>
   friend bool operator< (const flat_map<K1, T1, C1, A1>&,
                           const flat_map<K1, T1, C1, A1>&);

   private:
   mapped_type &priv_subscript(const key_type& k)
   {
      iterator i = lower_bound(k);
      // i->first is greater than or equivalent to k.
      if (i == end() || key_comp()(k, (*i).first)){
         container_detail::value_init<mapped_type> m;
         i = insert(i, impl_value_type(k, ::boost::move(m.m_t)));
      }
      return (*i).second;
   }
   mapped_type &priv_subscript(BOOST_RV_REF(key_type) mk) 
   {
      key_type &k = mk;
      iterator i = lower_bound(k);
      // i->first is greater than or equivalent to k.
      if (i == end() || key_comp()(k, (*i).first)){
         container_detail::value_init<mapped_type> m;
         i = insert(i, impl_value_type(boost::move(k), ::boost::move(m.m_t)));
      }
      return (*i).second;
   }
   /// @endcond
};

template <class Key, class T, class Pred, class A>
inline bool operator==(const flat_map<Key,T,Pred,A>& x, 
                       const flat_map<Key,T,Pred,A>& y) 
   {  return x.m_flat_tree == y.m_flat_tree;  }

template <class Key, class T, class Pred, class A>
inline bool operator<(const flat_map<Key,T,Pred,A>& x, 
                      const flat_map<Key,T,Pred,A>& y) 
   {  return x.m_flat_tree < y.m_flat_tree;   }

template <class Key, class T, class Pred, class A>
inline bool operator!=(const flat_map<Key,T,Pred,A>& x, 
                       const flat_map<Key,T,Pred,A>& y) 
   {  return !(x == y); }

template <class Key, class T, class Pred, class A>
inline bool operator>(const flat_map<Key,T,Pred,A>& x, 
                      const flat_map<Key,T,Pred,A>& y) 
   {  return y < x;  }

template <class Key, class T, class Pred, class A>
inline bool operator<=(const flat_map<Key,T,Pred,A>& x, 
                       const flat_map<Key,T,Pred,A>& y) 
   {  return !(y < x);  }

template <class Key, class T, class Pred, class A>
inline bool operator>=(const flat_map<Key,T,Pred,A>& x, 
                       const flat_map<Key,T,Pred,A>& y) 
   {  return !(x < y);  }

template <class Key, class T, class Pred, class A>
inline void swap(flat_map<Key,T,Pred,A>& x, 
                 flat_map<Key,T,Pred,A>& y) 
   {  x.swap(y);  }

/// @cond

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class K, class T, class C, class A>
struct has_trivial_destructor_after_move<boost::container::flat_map<K, T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
namespace container {

// Forward declaration of operators < and ==, needed for friend declaration.
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class flat_multimap;

template <class Key, class T, class Pred, class A>
inline bool operator==(const flat_multimap<Key,T,Pred,A>& x, 
                       const flat_multimap<Key,T,Pred,A>& y);

template <class Key, class T, class Pred, class A>
inline bool operator<(const flat_multimap<Key,T,Pred,A>& x, 
                      const flat_multimap<Key,T,Pred,A>& y);
/// @endcond

//! A flat_multimap is a kind of associative container that supports equivalent keys 
//! (possibly containing multiple copies of the same key value) and provides for 
//! fast retrieval of values of another type T based on the keys. The flat_multimap 
//! class supports random-access iterators.
//! 
//! A flat_multimap satisfies all of the requirements of a container and of a reversible 
//! container and of an associative container. For a 
//! flat_multimap<Key,T> the key_type is Key and the value_type is std::pair<Key,T>
//! (unlike std::multimap<Key, T> which value_type is std::pair<<b>const</b> Key, T>).
//!
//! Pred is the ordering function for Keys (e.g. <i>std::less<Key></i>).
//!
//! A is the allocator to allocate the value_types
//! (e.g. <i>allocator< std::pair<Key, T> ></i>).
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class Key, class T, class Pred = std::less< std::pair< Key, T> >, class A = std::allocator<T> >
#else
template <class Key, class T, class Pred, class A>
#endif
class flat_multimap 
{
   /// @cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(flat_multimap)
   typedef container_detail::flat_tree<Key, 
                           std::pair<Key, T>, 
                           container_detail::select1st< std::pair<Key, T> >, 
                           Pred, 
                           A> tree_t;
   //This is the real tree stored here. It's based on a movable pair
   typedef container_detail::flat_tree<Key, 
                           container_detail::pair<Key, T>, 
                           container_detail::select1st<container_detail::pair<Key, T> >, 
                           Pred, 
                           typename allocator_traits<A>::template portable_rebind_alloc
                              <container_detail::pair<Key, T> >::type> impl_tree_t;
   impl_tree_t m_flat_tree;  // flat tree representing flat_map

   typedef typename impl_tree_t::value_type              impl_value_type;
   typedef typename impl_tree_t::pointer                 impl_pointer;
   typedef typename impl_tree_t::const_pointer           impl_const_pointer;
   typedef typename impl_tree_t::reference               impl_reference;
   typedef typename impl_tree_t::const_reference         impl_const_reference;
   typedef typename impl_tree_t::value_compare           impl_value_compare;
   typedef typename impl_tree_t::iterator                impl_iterator;
   typedef typename impl_tree_t::const_iterator          impl_const_iterator;
   typedef typename impl_tree_t::reverse_iterator        impl_reverse_iterator;
   typedef typename impl_tree_t::const_reverse_iterator  impl_const_reverse_iterator;
   typedef typename impl_tree_t::allocator_type          impl_allocator_type;
   typedef allocator_traits<A>                           allocator_traits_type;

   /// @endcond

   public:

   // typedefs:
   typedef Key                                              key_type;
   typedef T                                                mapped_type;
   typedef Pred                                             key_compare;
   typedef typename std::pair<key_type, mapped_type>        value_type;
   typedef typename allocator_traits_type::pointer          pointer;
   typedef typename allocator_traits_type::const_pointer    const_pointer;
   typedef typename allocator_traits_type::reference        reference;
   typedef typename allocator_traits_type::const_reference  const_reference;
   typedef typename impl_tree_t::size_type                  size_type;
   typedef typename impl_tree_t::difference_type            difference_type;
   typedef container_detail::flat_tree_value_compare
      < Pred
      , container_detail::select1st< std::pair<Key, T> >
      , std::pair<Key, T> >                                 value_compare;

   typedef typename container_detail::
      get_flat_tree_iterators<pointer>::iterator            iterator;
   typedef typename container_detail::
      get_flat_tree_iterators<pointer>::const_iterator      const_iterator;
   typedef typename container_detail::
      get_flat_tree_iterators
         <pointer>::reverse_iterator                        reverse_iterator;
   typedef typename container_detail::
      get_flat_tree_iterators
         <pointer>::const_reverse_iterator                  const_reverse_iterator;
   typedef A                                                allocator_type;
   //Non-standard extension
   typedef A                                                stored_allocator_type;

   //! <b>Effects</b>: Default constructs an empty flat_map.
   //! 
   //! <b>Complexity</b>: Constant.
   flat_multimap() 
      : m_flat_tree() {}

   //! <b>Effects</b>: Constructs an empty flat_multimap using the specified comparison
   //!   object and allocator.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit flat_multimap(const Pred& comp,
                          const allocator_type& a = allocator_type())
      : m_flat_tree(comp, container_detail::force<impl_allocator_type>(a)) { }

   //! <b>Effects</b>: Constructs an empty flat_multimap using the specified comparison object
   //!   and allocator, and inserts elements from the range [first ,last ).
   //! 
   //! <b>Complexity</b>: Linear in N if the range [first ,last ) is already sorted using 
   //! comp and otherwise N logN, where N is last - first.
   template <class InputIterator>
   flat_multimap(InputIterator first, InputIterator last,
            const Pred& comp        = Pred(),
            const allocator_type& a = allocator_type())
      : m_flat_tree(comp, container_detail::force<impl_allocator_type>(a)) 
      { m_flat_tree.insert_equal(first, last); }

   //! <b>Effects</b>: Constructs an empty flat_multimap using the specified comparison object and 
   //! allocator, and inserts elements from the ordered range [first ,last). This function
   //! is more efficient than the normal range creation for ordered ranges.
   //!
   //! <b>Requires</b>: [first ,last) must be ordered according to the predicate.
   //! 
   //! <b>Complexity</b>: Linear in N.
   template <class InputIterator>
   flat_multimap(ordered_range_t, InputIterator first, InputIterator last,
            const Pred& comp        = Pred(),
            const allocator_type& a = allocator_type())
      : m_flat_tree(ordered_range, first, last, comp, a) 
   {}

   //! <b>Effects</b>: Copy constructs a flat_multimap.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_multimap(const flat_multimap<Key,T,Pred,A>& x) 
      : m_flat_tree(x.m_flat_tree) { }

   //! <b>Effects</b>: Move constructs a flat_multimap. Constructs *this using x's resources.
   //! 
   //! <b>Complexity</b>: Construct.
   //! 
   //! <b>Postcondition</b>: x is emptied.
   flat_multimap(BOOST_RV_REF(flat_multimap) x) 
      : m_flat_tree(boost::move(x.m_flat_tree))
   { }

   //! <b>Effects</b>: Makes *this a copy of x.
   //! 
   //! <b>Complexity</b>: Linear in x.size().
   flat_multimap<Key,T,Pred,A>& operator=(BOOST_COPY_ASSIGN_REF(flat_multimap) x) 
      {  m_flat_tree = x.m_flat_tree;   return *this;  }

   //! <b>Effects</b>: this->swap(x.get()).
   //! 
   //! <b>Complexity</b>: Constant.
   flat_multimap<Key,T,Pred,A>& operator=(BOOST_RV_REF(flat_multimap) mx) 
      {  m_flat_tree = boost::move(mx.m_flat_tree);   return *this;  }

   //! <b>Effects</b>: Returns the comparison object out
   //!   of which a was constructed.
   //! 
   //! <b>Complexity</b>: Constant.
   key_compare key_comp() const 
      { return container_detail::force<key_compare>(m_flat_tree.key_comp()); }

   //! <b>Effects</b>: Returns an object of value_compare constructed out
   //!   of the comparison object.
   //! 
   //! <b>Complexity</b>: Constant.
   value_compare value_comp() const 
      { return value_compare(container_detail::force<key_compare>(m_flat_tree.key_comp())); }

   //! <b>Effects</b>: Returns a copy of the Allocator that
   //!   was passed to the object's constructor.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const 
      { return container_detail::force<allocator_type>(m_flat_tree.get_allocator()); }

   const stored_allocator_type &get_stored_allocator() const 
      { return container_detail::force<stored_allocator_type>(m_flat_tree.get_stored_allocator()); }

   stored_allocator_type &get_stored_allocator()
      { return container_detail::force<stored_allocator_type>(m_flat_tree.get_stored_allocator()); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator begin() 
      { return container_detail::force_copy<iterator>(m_flat_tree.begin()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const 
      { return container_detail::force<const_iterator>(m_flat_tree.begin()); }

   //! <b>Effects</b>: Returns an iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator end() 
      { return container_detail::force_copy<iterator>(m_flat_tree.end()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator end() const 
      { return container_detail::force<const_iterator>(m_flat_tree.end()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() 
      { return container_detail::force<reverse_iterator>(m_flat_tree.rbegin()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.rbegin()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() 
      { return container_detail::force<reverse_iterator>(m_flat_tree.rend()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed container. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const 
      { return container_detail::force<const_reverse_iterator>(m_flat_tree.rend()); }

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
   void swap(flat_multimap& x)
   { m_flat_tree.swap(x.m_flat_tree); }

   //! <b>Effects</b>: Inserts x and returns the iterator pointing to the
   //!   newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const value_type& x) 
      { return container_detail::force_copy<iterator>(m_flat_tree.insert_equal(container_detail::force<impl_value_type>(x))); }

   //! <b>Effects</b>: Inserts a new value move-constructed from x and returns 
   //!   the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(BOOST_RV_REF(value_type) x) 
   { return container_detail::force_copy<iterator>(m_flat_tree.insert_equal(boost::move(x))); }

   //! <b>Effects</b>: Inserts a new value move-constructed from x and returns 
   //!   the iterator pointing to the newly inserted element. 
   //!
   //! <b>Complexity</b>: Logarithmic search time plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(BOOST_RV_REF(impl_value_type) x) 
      { return container_detail::force_copy<iterator>(m_flat_tree.insert_equal(boost::move(x))); }

   //! <b>Effects</b>: Inserts a copy of x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant time if the value
   //!   is to be inserted before p) plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, const value_type& x) 
      { return container_detail::force_copy<iterator>
         (m_flat_tree.insert_equal(container_detail::force<impl_const_iterator>(position), container_detail::force<impl_value_type>(x))); }

   //! <b>Effects</b>: Inserts a value move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant time if the value
   //!   is to be inserted before p) plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, BOOST_RV_REF(value_type) x) 
   {
      return container_detail::force_copy<iterator>
         (m_flat_tree.insert_equal(container_detail::force<impl_const_iterator>(position)
                                  , boost::move(x)));
   }

   //! <b>Effects</b>: Inserts a value move constructed from x in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant time if the value
   //!   is to be inserted before p) plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   iterator insert(const_iterator position, BOOST_RV_REF(impl_value_type) x) 
   {
      return container_detail::force_copy<iterator>(
         m_flat_tree.insert_equal(container_detail::force<impl_const_iterator>(position), boost::move(x)));
   }

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
      {  m_flat_tree.insert_equal(first, last); }

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
   {  return container_detail::force_copy<iterator>(m_flat_tree.emplace_equal(boost::forward<Args>(args)...)); }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the container.
   //!   p is a hint pointing to where the insert should start to search.
   //!
   //! <b>Returns</b>: An iterator pointing to the element with key equivalent
   //!   to the key of x.
   //!
   //! <b>Complexity</b>: Logarithmic search time (constant time if the value
   //!   is to be inserted before p) plus linear insertion
   //!   to the elements with bigger keys than x.
   //!
   //! <b>Note</b>: If an element is inserted it might invalidate elements.
   template <class... Args>
   iterator emplace_hint(const_iterator hint, Args&&... args)
   {
      return container_detail::force_copy<iterator>(m_flat_tree.emplace_hint_equal
         (container_detail::force<impl_const_iterator>(hint), boost::forward<Args>(args)...));
   }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                                 \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                            \
   {  return container_detail::force_copy<iterator>(m_flat_tree.emplace_equal                      \
               (BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))); }                         \
                                                                                                   \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)          \
   iterator emplace_hint(const_iterator hint                                                       \
                         BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))              \
   {  return container_detail::force_copy<iterator>(m_flat_tree.emplace_hint_equal                 \
            (container_detail::force<impl_const_iterator>(hint)                                    \
               BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))); }                 \
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
      { return container_detail::force_copy<iterator>(m_flat_tree.erase(container_detail::force<impl_const_iterator>(position))); }

   //! <b>Effects</b>: Erases all elements in the container with key equivalent to x.
   //!
   //! <b>Returns</b>: Returns the number of erased elements.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   size_type erase(const key_type& x) 
      { return m_flat_tree.erase(x); }

   //! <b>Effects</b>: Erases all the elements in the range [first, last).
   //!
   //! <b>Returns</b>: Returns last.
   //!
   //! <b>Complexity</b>: size()*N where N is the distance from first to last.
   //!
   //! <b>Complexity</b>: Logarithmic search time plus erasure time
   //!   linear to the elements with bigger keys.
   iterator erase(const_iterator first, const_iterator last)
      { return container_detail::force_copy<iterator>
         (m_flat_tree.erase(container_detail::force<impl_const_iterator>(first), container_detail::force<impl_const_iterator>(last))); }

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
      { return container_detail::force_copy<iterator>(m_flat_tree.find(x)); }

   //! <b>Returns</b>: An const_iterator pointing to an element with the key
   //!   equivalent to x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic.
   const_iterator find(const key_type& x) const 
      { return container_detail::force<const_iterator>(m_flat_tree.find(x)); }

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
      {return container_detail::force_copy<iterator>(m_flat_tree.lower_bound(x)); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key
   //!   not less than k, or a.end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator lower_bound(const key_type& x) const 
      {  return container_detail::force<const_iterator>(m_flat_tree.lower_bound(x));  }

   //! <b>Returns</b>: An iterator pointing to the first element with key not less
   //!   than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   iterator upper_bound(const key_type& x) 
      {return container_detail::force_copy<iterator>(m_flat_tree.upper_bound(x)); }

   //! <b>Returns</b>: A const iterator pointing to the first element with key
   //!   not less than x, or end() if such an element is not found.
   //!
   //! <b>Complexity</b>: Logarithmic
   const_iterator upper_bound(const key_type& x) const 
      {  return container_detail::force<const_iterator>(m_flat_tree.upper_bound(x)); }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<iterator,iterator> equal_range(const key_type& x) 
      {  return container_detail::force_copy<std::pair<iterator,iterator> >(m_flat_tree.equal_range(x));   }

   //! <b>Effects</b>: Equivalent to std::make_pair(this->lower_bound(k), this->upper_bound(k)).
   //!
   //! <b>Complexity</b>: Logarithmic
   std::pair<const_iterator,const_iterator> 
      equal_range(const key_type& x) const 
      {  return container_detail::force_copy<std::pair<const_iterator,const_iterator> >(m_flat_tree.equal_range(x));   }

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
   template <class K1, class T1, class C1, class A1>
   friend bool operator== (const flat_multimap<K1, T1, C1, A1>& x,
                           const flat_multimap<K1, T1, C1, A1>& y);

   template <class K1, class T1, class C1, class A1>
   friend bool operator< (const flat_multimap<K1, T1, C1, A1>& x,
                          const flat_multimap<K1, T1, C1, A1>& y);
   /// @endcond
};

template <class Key, class T, class Pred, class A>
inline bool operator==(const flat_multimap<Key,T,Pred,A>& x, 
                       const flat_multimap<Key,T,Pred,A>& y) 
   {  return x.m_flat_tree == y.m_flat_tree;  }

template <class Key, class T, class Pred, class A>
inline bool operator<(const flat_multimap<Key,T,Pred,A>& x, 
                      const flat_multimap<Key,T,Pred,A>& y) 
   {  return x.m_flat_tree < y.m_flat_tree;   }

template <class Key, class T, class Pred, class A>
inline bool operator!=(const flat_multimap<Key,T,Pred,A>& x, 
                       const flat_multimap<Key,T,Pred,A>& y) 
   {  return !(x == y);  }

template <class Key, class T, class Pred, class A>
inline bool operator>(const flat_multimap<Key,T,Pred,A>& x, 
                      const flat_multimap<Key,T,Pred,A>& y) 
   {  return y < x;  }

template <class Key, class T, class Pred, class A>
inline bool operator<=(const flat_multimap<Key,T,Pred,A>& x, 
                       const flat_multimap<Key,T,Pred,A>& y) 
   {  return !(y < x);  }

template <class Key, class T, class Pred, class A>
inline bool operator>=(const flat_multimap<Key,T,Pred,A>& x, 
                       const flat_multimap<Key,T,Pred,A>& y) 
   {  return !(x < y);  }

template <class Key, class T, class Pred, class A>
inline void swap(flat_multimap<Key,T,Pred,A>& x, flat_multimap<Key,T,Pred,A>& y) 
   {  x.swap(y);  }

}}

/// @cond

namespace boost {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class K, class T, class C, class A>
struct has_trivial_destructor_after_move< boost::container::flat_multimap<K, T, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
}  //namespace boost { 

/// @endcond

#include <boost/container/detail/config_end.hpp>

#endif /* BOOST_CONTAINER_FLAT_MAP_HPP */
