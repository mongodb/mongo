/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_TRIE_SET_HPP
#define BOOST_INTRUSIVE_TRIE_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/treap.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/move/move.hpp>
#include <iterator>

namespace boost {
namespace intrusive {

//! The class template treap_set is an intrusive container, that mimics most of
//! the interface of std::set as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>,
//! \c compare<> and \c priority_compare<>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidOrKeyComp, class VoidOrPrioComp, class SizeType, bool ConstantTimeSize>
#endif
class treap_set_impl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public treap_impl<ValueTraits, VoidOrKeyComp, VoidOrPrioComp, SizeType, ConstantTimeSize>
#endif
{
   /// @cond
   typedef treap_impl<ValueTraits, VoidOrKeyComp, VoidOrPrioComp, SizeType, ConstantTimeSize> tree_type;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_set_impl)

   typedef tree_type implementation_defined;
   /// @endcond

   public:
   typedef typename implementation_defined::value_type               value_type;
   typedef typename implementation_defined::value_traits             value_traits;
   typedef typename implementation_defined::pointer                  pointer;
   typedef typename implementation_defined::const_pointer            const_pointer;
   typedef typename implementation_defined::reference                reference;
   typedef typename implementation_defined::const_reference          const_reference;
   typedef typename implementation_defined::difference_type          difference_type;
   typedef typename implementation_defined::size_type                size_type;
   typedef typename implementation_defined::value_compare            value_compare;
   typedef typename implementation_defined::priority_compare         priority_compare;
   typedef typename implementation_defined::key_compare              key_compare;
   typedef typename implementation_defined::iterator                 iterator;
   typedef typename implementation_defined::const_iterator           const_iterator;
   typedef typename implementation_defined::reverse_iterator         reverse_iterator;
   typedef typename implementation_defined::const_reverse_iterator   const_reverse_iterator;
   typedef typename implementation_defined::insert_commit_data       insert_commit_data;
   typedef typename implementation_defined::node_traits              node_traits;
   typedef typename implementation_defined::node                     node;
   typedef typename implementation_defined::node_ptr                 node_ptr;
   typedef typename implementation_defined::const_node_ptr           const_node_ptr;
   typedef typename implementation_defined::node_algorithms          node_algorithms;

   static const bool constant_time_size = implementation_defined::constant_time_size;

   public:
   //! <b>Effects</b>: Constructs an empty treap_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare object throws.
   explicit treap_set_impl( const value_compare &cmp      = value_compare()
                          , const priority_compare &pcmp  = priority_compare()
                          , const value_traits &v_traits  = value_traits())
      :  tree_type(cmp, pcmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty treap_set and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is std::distance(last, first).
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws.
   template<class Iterator>
   treap_set_impl( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const priority_compare &pcmp  = priority_compare()
           , const value_traits &v_traits = value_traits())
      : tree_type(true, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!
   treap_set_impl(BOOST_RV_REF(treap_set_impl) x)
      :  tree_type(::boost::move(static_cast<tree_type&>(x)))
   {}

   //! <b>Effects</b>: to-do
   //!
   treap_set_impl& operator=(BOOST_RV_REF(treap_set_impl) x)
   {  return static_cast<treap_set_impl&>(tree_type::operator=(::boost::move(static_cast<tree_type&>(x))));  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::~treap()
   ~treap_set_impl();

   //! @copydoc ::boost::intrusive::treap::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::treap::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::treap::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::treap::end()
   iterator end();

   //! @copydoc ::boost::intrusive::treap::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::treap::cend()const
   const_iterator cend() const;

   //! @copydoc ::boost::intrusive::treap::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::treap::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::treap::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::treap::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::treap::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::treap::crend()const
   const_reverse_iterator crend() const;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(iterator)
   static treap_set_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(const_iterator)
   static const treap_set_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(iterator)
   static treap_set_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(const_iterator)
   static const treap_set_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::treap::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::treap::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::treap::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::treap::size()const
   size_type size() const;

   //! @copydoc ::boost::intrusive::treap::swap
   void swap(treap_set_impl& other);

   //! @copydoc ::boost::intrusive::treap::clone_from
   template <class Cloner, class Disposer>
   void clone_from(const treap_set_impl &src, Cloner cloner, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::top()
   iterator top();

   //! @copydoc ::boost::intrusive::treap::top()const
   const_iterator top() const;

   //! @copydoc ::boost::intrusive::treap::ctop()const
   const_iterator ctop() const;

   //! @copydoc ::boost::intrusive::treap::rtop()
   reverse_iterator rtop();

   //! @copydoc ::boost::intrusive::treap::rtop()const
   const_reverse_iterator rtop() const;

   //! @copydoc ::boost::intrusive::treap::crtop()const
   const_reverse_iterator crtop() const;

   //! @copydoc ::boost::intrusive::treap::crtop() const
   priority_compare priority_comp() const;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::insert_unique(reference)
   std::pair<iterator, bool> insert(reference value)
   {  return tree_type::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique(const_iterator,reference)
   iterator insert(const_iterator hint, reference value)
   {  return tree_type::insert_unique(hint, value);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const KeyType&,KeyValueCompare,KeyValuePrioCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare, class KeyValuePrioCompare>
   std::pair<iterator, bool> insert_check
      ( const KeyType &key, KeyValueCompare key_value_comp, KeyValuePrioCompare key_value_pcomp
      , insert_commit_data &commit_data)
   {  return tree_type::insert_unique_check(key, key_value_comp, key_value_pcomp, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const_iterator,const KeyType&,KeyValueCompare,KeyValuePrioCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare, class KeyValuePrioCompare>
   std::pair<iterator, bool> insert_check
      ( const_iterator hint, const KeyType &key
      , KeyValueCompare key_value_comp, KeyValuePrioCompare key_value_pcomp
      , insert_commit_data &commit_data)
   {  return tree_type::insert_unique_check(hint, key, key_value_comp, key_value_pcomp, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_type::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique_commit
   iterator insert_commit(reference value, const insert_commit_data &commit_data)
   {  return tree_type::insert_unique_commit(value, commit_data);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::insert_before
   iterator insert_before(const_iterator pos, reference value);

   //! @copydoc ::boost::intrusive::treap::push_back
   void push_back(reference value);

   //! @copydoc ::boost::intrusive::treap::push_front
   void push_front(reference value);

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator)
   iterator erase(const_iterator i);

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e);

   //! @copydoc ::boost::intrusive::treap::erase(const_reference)
   size_type erase(const_reference value);

   //! @copydoc ::boost::intrusive::treap::erase(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_reference, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const KeyType&,KeyValueCompare,Disposer)
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::clear
   void clear();

   //! @copydoc ::boost::intrusive::treap::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::count(const_reference)const
   size_type count(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const;
   
   //! @copydoc ::boost::intrusive::treap::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::treap::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::treap::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::treap::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::treap::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::treap::remove_node
   void remove_node(reference value);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};


//! Helper metafunction to define a \c treap_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void>
#endif
struct make_treap_set
{
   typedef typename pack_options
      < treap_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;

   typedef treap_set_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::priority
         , typename packed_options::size_type
         , packed_options::constant_time_size
         > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4>
#else
template<class T, class ...Options>
#endif
class treap_set
   :  public make_treap_set<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_treap_set
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_set)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::priority_compare   priority_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   explicit treap_set( const value_compare &cmp    = value_compare()
                     , const priority_compare &pcmp = priority_compare()
                     , const value_traits &v_traits = value_traits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   treap_set( Iterator b, Iterator e
      , const value_compare &cmp = value_compare()
      , const priority_compare &pcmp = priority_compare()
      , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, pcmp, v_traits)
   {}

   treap_set(BOOST_RV_REF(treap_set) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   treap_set& operator=(BOOST_RV_REF(treap_set) x)
   {  return static_cast<treap_set &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static treap_set &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<treap_set &>(Base::container_from_end_iterator(end_iterator));   }

   static const treap_set &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const treap_set &>(Base::container_from_end_iterator(end_iterator));   }

   static treap_set &container_from_iterator(iterator it)
   {  return static_cast<treap_set &>(Base::container_from_iterator(it));   }

   static const treap_set &container_from_iterator(const_iterator it)
   {  return static_cast<const treap_set &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template treap_multiset is an intrusive container, that mimics most of
//! the interface of std::treap_multiset as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>,
//! \c compare<> and \c priority_compare<>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidOrKeyComp, class VoidOrPrioComp, class SizeType, bool ConstantTimeSize>
#endif
class treap_multiset_impl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public treap_impl<ValueTraits, VoidOrKeyComp, VoidOrPrioComp, SizeType, ConstantTimeSize>
#endif
{
   /// @cond
   typedef treap_impl<ValueTraits, VoidOrKeyComp, VoidOrPrioComp, SizeType, ConstantTimeSize> tree_type;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_multiset_impl)

   typedef tree_type implementation_defined;
   /// @endcond

   public:
   typedef typename implementation_defined::value_type               value_type;
   typedef typename implementation_defined::value_traits             value_traits;
   typedef typename implementation_defined::pointer                  pointer;
   typedef typename implementation_defined::const_pointer            const_pointer;
   typedef typename implementation_defined::reference                reference;
   typedef typename implementation_defined::const_reference          const_reference;
   typedef typename implementation_defined::difference_type          difference_type;
   typedef typename implementation_defined::size_type                size_type;
   typedef typename implementation_defined::value_compare            value_compare;
   typedef typename implementation_defined::priority_compare         priority_compare;
   typedef typename implementation_defined::key_compare              key_compare;
   typedef typename implementation_defined::iterator                 iterator;
   typedef typename implementation_defined::const_iterator           const_iterator;
   typedef typename implementation_defined::reverse_iterator         reverse_iterator;
   typedef typename implementation_defined::const_reverse_iterator   const_reverse_iterator;
   typedef typename implementation_defined::insert_commit_data       insert_commit_data;
   typedef typename implementation_defined::node_traits              node_traits;
   typedef typename implementation_defined::node                     node;
   typedef typename implementation_defined::node_ptr                 node_ptr;
   typedef typename implementation_defined::const_node_ptr           const_node_ptr;
   typedef typename implementation_defined::node_algorithms          node_algorithms;

   static const bool constant_time_size = implementation_defined::constant_time_size;

   public:
   //! <b>Effects</b>: Constructs an empty treap_multiset.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare object throws.
   explicit treap_multiset_impl( const value_compare &cmp      = value_compare()
                          , const priority_compare &pcmp  = priority_compare()
                          , const value_traits &v_traits  = value_traits())
      :  tree_type(cmp, pcmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty treap_multiset and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is std::distance(last, first).
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws.
   template<class Iterator>
   treap_multiset_impl( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const priority_compare &pcmp  = priority_compare()
           , const value_traits &v_traits = value_traits())
      : tree_type(false, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!
   treap_multiset_impl(BOOST_RV_REF(treap_multiset_impl) x)
      :  tree_type(::boost::move(static_cast<tree_type&>(x)))
   {}

   //! <b>Effects</b>: to-do
   //!
   treap_multiset_impl& operator=(BOOST_RV_REF(treap_multiset_impl) x)
   {  return static_cast<treap_multiset_impl&>(tree_type::operator=(::boost::move(static_cast<tree_type&>(x))));  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::~treap()
   ~treap_multiset_impl();

   //! @copydoc ::boost::intrusive::treap::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::treap::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::treap::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::treap::end()
   iterator end();

   //! @copydoc ::boost::intrusive::treap::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::treap::cend()const
   const_iterator cend() const;

   //! @copydoc ::boost::intrusive::treap::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::treap::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::treap::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::treap::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::treap::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::treap::crend()const
   const_reverse_iterator crend() const;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(iterator)
   static treap_multiset_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(const_iterator)
   static const treap_multiset_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(iterator)
   static treap_multiset_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(const_iterator)
   static const treap_multiset_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::treap::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::treap::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::treap::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::treap::size()const
   size_type size() const;

   //! @copydoc ::boost::intrusive::treap::swap
   void swap(treap_multiset_impl& other);

   //! @copydoc ::boost::intrusive::treap::clone_from
   template <class Cloner, class Disposer>
   void clone_from(const treap_multiset_impl &src, Cloner cloner, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::top()
   iterator top();

   //! @copydoc ::boost::intrusive::treap::top()const
   const_iterator top() const;

   //! @copydoc ::boost::intrusive::treap::ctop()const
   const_iterator ctop() const;

   //! @copydoc ::boost::intrusive::treap::rtop()
   reverse_iterator rtop();

   //! @copydoc ::boost::intrusive::treap::rtop()const
   const_reverse_iterator rtop() const;

   //! @copydoc ::boost::intrusive::treap::crtop()const
   const_reverse_iterator crtop() const;

   //! @copydoc ::boost::intrusive::treap::crtop() const
   priority_compare priority_comp() const;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::insert_equal(reference)
   iterator insert(reference value)
   {  return tree_type::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::treap::insert_equal(const_iterator,reference)
   iterator insert(const_iterator hint, reference value)
   {  return tree_type::insert_equal(hint, value);  }

   //! @copydoc ::boost::intrusive::treap::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_type::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::insert_before
   iterator insert_before(const_iterator pos, reference value);

   //! @copydoc ::boost::intrusive::treap::push_back
   void push_back(reference value);

   //! @copydoc ::boost::intrusive::treap::push_front
   void push_front(reference value);

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator)
   iterator erase(const_iterator i);

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e);

   //! @copydoc ::boost::intrusive::treap::erase(const_reference)
   size_type erase(const_reference value);

   //! @copydoc ::boost::intrusive::treap::erase(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_reference, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const KeyType&,KeyValueCompare,Disposer)
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::clear
   void clear();

   //! @copydoc ::boost::intrusive::treap::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::count(const_reference)const
   size_type count(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const;
   
   //! @copydoc ::boost::intrusive::treap::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::treap::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::treap::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::treap::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::treap::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::treap::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::treap::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::treap::remove_node
   void remove_node(reference value);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};


//! Helper metafunction to define a \c treap_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void>
#endif
struct make_treap_multiset
{
   typedef typename pack_options
      < treap_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;

   typedef treap_multiset_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::priority
         , typename packed_options::size_type
         , packed_options::constant_time_size
         > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4>
#else
template<class T, class ...Options>
#endif
class treap_multiset
   :  public make_treap_multiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_treap_multiset
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_multiset)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::priority_compare   priority_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   explicit treap_multiset( const value_compare &cmp    = value_compare()
                          , const priority_compare &pcmp = priority_compare()
                          , const value_traits &v_traits = value_traits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   treap_multiset( Iterator b, Iterator e
      , const value_compare &cmp = value_compare()
      , const priority_compare &pcmp = priority_compare()
      , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, pcmp, v_traits)
   {}

   treap_multiset(BOOST_RV_REF(treap_multiset) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   treap_multiset& operator=(BOOST_RV_REF(treap_multiset) x)
   {  return static_cast<treap_multiset &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static treap_multiset &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<treap_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static const treap_multiset &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const treap_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static treap_multiset &container_from_iterator(iterator it)
   {  return static_cast<treap_multiset &>(Base::container_from_iterator(it));   }

   static const treap_multiset &container_from_iterator(const_iterator it)
   {  return static_cast<const treap_multiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TRIE_SET_HPP
