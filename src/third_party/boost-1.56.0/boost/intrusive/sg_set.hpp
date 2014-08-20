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
#ifndef BOOST_INTRUSIVE_SG_SET_HPP
#define BOOST_INTRUSIVE_SG_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/sgtree.hpp>
#include <iterator>
#include <boost/move/move.hpp>

namespace boost {
namespace intrusive {

//! The class template sg_set is an intrusive container, that mimics most of
//! the interface of std::sg_set as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c floating_point<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class Compare, class SizeType, bool FloatingPoint, typename HeaderHolder>
#endif
class sg_set_impl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public sgtree_impl<ValueTraits, Compare, SizeType, FloatingPoint, HeaderHolder>
#endif
{
   /// @cond
   typedef sgtree_impl<ValueTraits, Compare, SizeType, FloatingPoint, HeaderHolder> tree_type;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(sg_set_impl)

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

   static const bool constant_time_size = tree_type::constant_time_size;

   public:
   //! @copydoc ::boost::intrusive::sgtree::sgtree(const value_compare &,const value_traits &)
   explicit sg_set_impl( const value_compare &cmp = value_compare()
                    , const value_traits &v_traits = value_traits())
      :  tree_type(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::sgtree::sgtree(bool,Iterator,Iterator,const value_compare &,const value_traits &)
   template<class Iterator>
   sg_set_impl( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      : tree_type(true, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::sgtree::sgtree(sgtree &&)
   sg_set_impl(BOOST_RV_REF(sg_set_impl) x)
      :  tree_type(::boost::move(static_cast<tree_type&>(x)))
   {}

   //! @copydoc ::boost::intrusive::sgtree::operator=(sgtree &&)
   sg_set_impl& operator=(BOOST_RV_REF(sg_set_impl) x)
   {  return static_cast<sg_set_impl&>(tree_type::operator=(::boost::move(static_cast<tree_type&>(x)))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::sgtree::~sgtree()
   ~sg_set_impl();

   //! @copydoc ::boost::intrusive::sgtree::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::sgtree::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::sgtree::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::end()
   iterator end();

   //! @copydoc ::boost::intrusive::sgtree::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::sgtree::cend()const
   const_iterator cend() const;

   //! @copydoc ::boost::intrusive::sgtree::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::sgtree::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::sgtree::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::sgtree::crend()const
   const_reverse_iterator crend() const;

   //! @copydoc ::boost::intrusive::sgtree::container_from_end_iterator(iterator)
   static sg_set_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::sgtree::container_from_end_iterator(const_iterator)
   static const sg_set_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::sgtree::container_from_iterator(iterator)
   static sg_set_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::sgtree::container_from_iterator(const_iterator)
   static const sg_set_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::sgtree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::sgtree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::sgtree::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::sgtree::size()const
   size_type size() const;

   //! @copydoc ::boost::intrusive::sgtree::swap
   void swap(sg_set_impl& other);

   //! @copydoc ::boost::intrusive::sgtree::clone_from
   template <class Cloner, class Disposer>
   void clone_from(const sg_set_impl &src, Cloner cloner, Disposer disposer);
   
   #endif   //#ifdef BOOST_iNTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::sgtree::insert_unique(reference)
   std::pair<iterator, bool> insert(reference value)
   {  return tree_type::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::sgtree::insert_unique(const_iterator,reference)
   iterator insert(const_iterator hint, reference value)
   {  return tree_type::insert_unique(hint, value);  }

   //! @copydoc ::boost::intrusive::sgtree::insert_unique_check(const KeyType&,KeyValueCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {  return tree_type::insert_unique_check(key, key_value_comp, commit_data); }

   //! @copydoc ::boost::intrusive::sgtree::insert_unique_check(const_iterator,const KeyType&,KeyValueCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {  return tree_type::insert_unique_check(hint, key, key_value_comp, commit_data); }

   //! @copydoc ::boost::intrusive::sgtree::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_type::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::sgtree::insert_unique_commit
   iterator insert_commit(reference value, const insert_commit_data &commit_data)
   {  return tree_type::insert_unique_commit(value, commit_data);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::sgtree::insert_before
   iterator insert_before(const_iterator pos, reference value);

   //! @copydoc ::boost::intrusive::sgtree::push_back
   void push_back(reference value);

   //! @copydoc ::boost::intrusive::sgtree::push_front
   void push_front(reference value);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_iterator)
   iterator erase(const_iterator i);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_reference)
   size_type erase(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::erase(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_reference, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const KeyType&,KeyValueCompare,Disposer)
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::clear
   void clear();

   //! @copydoc ::boost::intrusive::sgtree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer);

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::sgtree::count(const_reference)const
   size_type count(const_reference value) const
   {  return static_cast<size_type>(this->tree_type::find(value) != this->tree_type::cend()); }

   //! @copydoc ::boost::intrusive::sgtree::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const
   {  return static_cast<size_type>(this->tree_type::find(key, comp) != this->tree_type::cend()); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return this->tree_type::lower_bound_range(value); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp)
   {  return this->tree_type::lower_bound_range(key, comp); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return this->tree_type::lower_bound_range(value); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const
   {  return this->tree_type::lower_bound_range(key, comp); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::sgtree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::sgtree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::sgtree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::sgtree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::sgtree::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::sgtree::remove_node
   void remove_node(reference value);

   //! @copydoc ::boost::intrusive::sgtree::rebalance
   void rebalance();

   //! @copydoc ::boost::intrusive::sgtree::rebalance_subtree
   iterator rebalance_subtree(iterator root);

   //! @copydoc ::boost::intrusive::sgtree::balance_factor()
   float balance_factor() const;

   //! @copydoc ::boost::intrusive::sgtree::balance_factor(float)
   void balance_factor(float new_alpha);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const sg_set_impl<T, Options...> &x, const sg_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const sg_set_impl<T, Options...> &x, const sg_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const sg_set_impl<T, Options...> &x, const sg_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const sg_set_impl<T, Options...> &x, const sg_set_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(sg_set_impl<T, Options...> &x, sg_set_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c sg_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void>
#endif
struct make_sg_set
{
   /// @cond
   typedef typename pack_options
      < sgtree_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;
   typedef typename detail::get_header_holder_type
      < value_traits, typename packed_options::header_holder_type >::type header_holder_type;

   typedef sg_set_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::size_type
         , packed_options::floating_point
         , header_holder_type
         > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5>
#else
template<class T, class ...Options>
#endif
class sg_set
   :  public make_sg_set<T,
   #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
   O1, O2, O3, O4, O5
   #else
   Options...
   #endif
   >::type
{
   typedef typename make_sg_set
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(sg_set)
   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   explicit sg_set( const value_compare &cmp = value_compare()
                  , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   sg_set( Iterator b, Iterator e
      , const value_compare &cmp = value_compare()
      , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, v_traits)
   {}

   sg_set(BOOST_RV_REF(sg_set) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   sg_set& operator=(BOOST_RV_REF(sg_set) x)
   {  return static_cast<sg_set &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static sg_set &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<sg_set &>(Base::container_from_end_iterator(end_iterator));   }

   static const sg_set &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const sg_set &>(Base::container_from_end_iterator(end_iterator));   }

   static sg_set &container_from_iterator(iterator it)
   {  return static_cast<sg_set &>(Base::container_from_iterator(it));   }

   static const sg_set &container_from_iterator(const_iterator it)
   {  return static_cast<const sg_set &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template sg_multiset is an intrusive container, that mimics most of
//! the interface of std::sg_multiset as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c floating_point<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class Compare, class SizeType, bool FloatingPoint, typename HeaderHolder>
#endif
class sg_multiset_impl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public sgtree_impl<ValueTraits, Compare, SizeType, FloatingPoint, HeaderHolder>
#endif
{
   /// @cond
   typedef sgtree_impl<ValueTraits, Compare, SizeType, FloatingPoint, HeaderHolder> tree_type;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(sg_multiset_impl)
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

   static const bool constant_time_size = tree_type::constant_time_size;

   public:
   //! @copydoc ::boost::intrusive::sgtree::sgtree(const value_compare &,const value_traits &)
   explicit sg_multiset_impl( const value_compare &cmp = value_compare()
                         , const value_traits &v_traits = value_traits())
      :  tree_type(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::sgtree::sgtree(bool,Iterator,Iterator,const value_compare &,const value_traits &)
   template<class Iterator>
   sg_multiset_impl( Iterator b, Iterator e
                , const value_compare &cmp = value_compare()
                , const value_traits &v_traits = value_traits())
      : tree_type(false, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::sgtree::sgtree(sgtree &&)
   sg_multiset_impl(BOOST_RV_REF(sg_multiset_impl) x)
      :  tree_type(::boost::move(static_cast<tree_type&>(x)))
   {}

   //! @copydoc ::boost::intrusive::sgtree::operator=(sgtree &&)
   sg_multiset_impl& operator=(BOOST_RV_REF(sg_multiset_impl) x)
   {  return static_cast<sg_multiset_impl&>(tree_type::operator=(::boost::move(static_cast<tree_type&>(x)))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::sgtree::~sgtree()
   ~sg_multiset_impl();

   //! @copydoc ::boost::intrusive::sgtree::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::sgtree::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::sgtree::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::end()
   iterator end();

   //! @copydoc ::boost::intrusive::sgtree::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::sgtree::cend()const
   const_iterator cend() const;

   //! @copydoc ::boost::intrusive::sgtree::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::sgtree::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::sgtree::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::sgtree::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::sgtree::crend()const
   const_reverse_iterator crend() const;

   //! @copydoc ::boost::intrusive::sgtree::container_from_end_iterator(iterator)
   static sg_multiset_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::sgtree::container_from_end_iterator(const_iterator)
   static const sg_multiset_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::sgtree::container_from_iterator(iterator)
   static sg_multiset_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::sgtree::container_from_iterator(const_iterator)
   static const sg_multiset_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::sgtree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::sgtree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::sgtree::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::sgtree::size()const
   size_type size() const;

   //! @copydoc ::boost::intrusive::sgtree::swap
   void swap(sg_multiset_impl& other);

   //! @copydoc ::boost::intrusive::sgtree::clone_from
   template <class Cloner, class Disposer>
   void clone_from(const sg_multiset_impl &src, Cloner cloner, Disposer disposer);

   #endif   //#ifdef BOOST_iNTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::sgtree::insert_equal(reference)
   iterator insert(reference value)
   {  return tree_type::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::sgtree::insert_equal(const_iterator,reference)
   iterator insert(const_iterator hint, reference value)
   {  return tree_type::insert_equal(hint, value);  }

   //! @copydoc ::boost::intrusive::sgtree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_type::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::sgtree::insert_before
   iterator insert_before(const_iterator pos, reference value);

   //! @copydoc ::boost::intrusive::sgtree::push_back
   void push_back(reference value);

   //! @copydoc ::boost::intrusive::sgtree::push_front
   void push_front(reference value);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_iterator)
   iterator erase(const_iterator i);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e);

   //! @copydoc ::boost::intrusive::sgtree::erase(const_reference)
   size_type erase(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::erase(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const_reference, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::erase_and_dispose(const KeyType&,KeyValueCompare,Disposer)
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::clear
   void clear();

   //! @copydoc ::boost::intrusive::sgtree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer);

   //! @copydoc ::boost::intrusive::sgtree::count(const_reference)const
   size_type count(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const;
   
   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::sgtree::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::sgtree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::sgtree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::sgtree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::sgtree::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::sgtree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::sgtree::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::sgtree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::sgtree::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::sgtree::remove_node
   void remove_node(reference value);

   //! @copydoc ::boost::intrusive::sgtree::rebalance
   void rebalance();

   //! @copydoc ::boost::intrusive::sgtree::rebalance_subtree
   iterator rebalance_subtree(iterator root);

   //! @copydoc ::boost::intrusive::sgtree::balance_factor()
   float balance_factor() const;

   //! @copydoc ::boost::intrusive::sgtree::balance_factor(float)
   void balance_factor(float new_alpha);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const sg_multiset_impl<T, Options...> &x, const sg_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const sg_multiset_impl<T, Options...> &x, const sg_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const sg_multiset_impl<T, Options...> &x, const sg_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const sg_multiset_impl<T, Options...> &x, const sg_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(sg_multiset_impl<T, Options...> &x, sg_multiset_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c sg_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void>
#endif
struct make_sg_multiset
{
   /// @cond
   typedef typename pack_options
      < sgtree_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;
   typedef typename detail::get_header_holder_type
      < value_traits, typename packed_options::header_holder_type >::type header_holder_type;

   typedef sg_multiset_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::size_type
         , packed_options::floating_point
         , header_holder_type
         > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5>
#else
template<class T, class ...Options>
#endif
class sg_multiset
   :  public make_sg_multiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_sg_multiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(sg_multiset)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   sg_multiset( const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   sg_multiset( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, v_traits)
   {}

   sg_multiset(BOOST_RV_REF(sg_multiset) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   sg_multiset& operator=(BOOST_RV_REF(sg_multiset) x)
   {  return static_cast<sg_multiset &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static sg_multiset &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<sg_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static const sg_multiset &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const sg_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static sg_multiset &container_from_iterator(iterator it)
   {  return static_cast<sg_multiset &>(Base::container_from_iterator(it));   }

   static const sg_multiset &container_from_iterator(const_iterator it)
   {  return static_cast<const sg_multiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SG_SET_HPP
