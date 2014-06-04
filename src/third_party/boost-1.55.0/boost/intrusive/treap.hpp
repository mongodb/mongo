/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_TREAP_HPP
#define BOOST_INTRUSIVE_TREAP_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/bs_set_hook.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/treap_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/move/move.hpp>
#include <boost/intrusive/priority_compare.hpp>

namespace boost {
namespace intrusive {

/// @cond

struct treap_defaults
{
   typedef detail::default_bstree_hook proto_value_traits;
   static const bool constant_time_size = true;
   typedef std::size_t size_type;
   typedef void compare;
   typedef void priority;
};

/// @endcond

//! The class template treap is an intrusive treap container that
//! is used to construct intrusive set and multiset containers. The no-throw
//! guarantee holds only, if the value_compare object and priority_compare object
//! don't throw.
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
class treap_impl
   /// @cond
   :  public bstree_impl<ValueTraits, VoidOrKeyComp, SizeType, ConstantTimeSize, BsTreeAlgorithms>
   ,  public detail::ebo_functor_holder
         < typename get_prio
            < VoidOrPrioComp
            , typename bstree_impl
               <ValueTraits, VoidOrKeyComp, SizeType, ConstantTimeSize, BsTreeAlgorithms>::value_type>::type 
         >
   /// @endcond
{
   public:
   typedef ValueTraits                                               value_traits;
   /// @cond
   typedef bstree_impl< ValueTraits, VoidOrKeyComp, SizeType
                      , ConstantTimeSize, BsTreeAlgorithms>          tree_type;
   typedef tree_type                                                 implementation_defined;
   typedef typename tree_type::real_value_traits                     real_value_traits;
   typedef get_prio
               < VoidOrPrioComp
               , typename tree_type::value_type>                     get_prio_type;

   typedef detail::ebo_functor_holder
      <typename get_prio_type::type>                                 prio_base;

   /// @endcond

   typedef typename implementation_defined::pointer                  pointer;
   typedef typename implementation_defined::const_pointer            const_pointer;
   typedef typename implementation_defined::value_type               value_type;
   typedef typename implementation_defined::key_type                 key_type;
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
   typedef typename implementation_defined::node_traits              node_traits;
   typedef typename implementation_defined::node                     node;
   typedef typename implementation_defined::node_ptr                 node_ptr;
   typedef typename implementation_defined::const_node_ptr           const_node_ptr;
   typedef BOOST_INTRUSIVE_IMPDEF(treap_algorithms<node_traits>)     node_algorithms;
   typedef BOOST_INTRUSIVE_IMPDEF(typename get_prio_type::type)      priority_compare;

   static const bool constant_time_size      = implementation_defined::constant_time_size;
   static const bool stateful_value_traits   = implementation_defined::stateful_value_traits;
   static const bool safemode_or_autounlink = is_safe_autounlink<real_value_traits::link_mode>::value;

   /// @cond
   private:

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_impl)

   const priority_compare &priv_pcomp() const
   {  return static_cast<const prio_base&>(*this).get();  }

   priority_compare &priv_pcomp()
   {  return static_cast<prio_base&>(*this).get();  }

   /// @endcond

   public:
   typedef typename node_algorithms::insert_commit_data insert_commit_data;

   //! <b>Effects</b>: Constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare/priority_compare objects throw. Basic guarantee.
   explicit treap_impl( const value_compare &cmp    = value_compare()
                      , const priority_compare &pcmp = priority_compare()
                      , const value_traits &v_traits = value_traits())
      : tree_type(cmp, v_traits), prio_base(pcmp)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty container and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is the distance between first and last.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare/priority_compare objects
   //!   throw. Basic guarantee.
   template<class Iterator>
   treap_impl( bool unique, Iterator b, Iterator e
            , const value_compare &cmp     = value_compare()
            , const priority_compare &pcmp = priority_compare()
            , const value_traits &v_traits = value_traits())
      : tree_type(cmp, v_traits), prio_base(pcmp)
   {
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   treap_impl(BOOST_RV_REF(treap_impl) x)
      : tree_type(::boost::move(static_cast<tree_type&>(x)))
      , prio_base(::boost::move(x.priv_pcomp()))
   {}

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   treap_impl& operator=(BOOST_RV_REF(treap_impl) x)
   {  this->swap(x); return *this;  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~treap_impl();

   //! @copydoc ::boost::intrusive::bstree::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::bstree::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::bstree::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::bstree::end()
   iterator end();

   //! @copydoc ::boost::intrusive::bstree::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::bstree::cend()const
   const_iterator cend() const;
   #endif

   //! <b>Effects</b>: Returns an iterator pointing to the highest priority object of the treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator top()
   {  return this->empty() ? this->end() : iterator(node_traits::get_parent(this->tree_type::header_ptr()), this);   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the treap..
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator top() const
   {  return this->ctop();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the treap..
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator ctop() const
   {  return this->empty() ? this->cend() : const_iterator(node_traits::get_parent(this->tree_type::header_ptr()), this);   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::bstree::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::bstree::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::bstree::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::bstree::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::bstree::crend()const
   const_reverse_iterator crend() const;
   #endif

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the highest priority object of the
   //!    reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   reverse_iterator rtop()
   {  return reverse_iterator(this->top());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority objec
   //!    of the reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rtop() const
   {  return const_reverse_iterator(this->top());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority object
   //!    of the reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crtop() const
   {  return const_reverse_iterator(this->top());  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(iterator)
   static treap_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const treap_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static treap_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const treap_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns the priority_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If priority_compare copy-constructor throws.
   priority_compare priority_comp() const
   {  return this->priv_pcomp();   }

   //! <b>Effects</b>: Swaps the contents of two treaps.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the comparison functor's swap call throws.
   void swap(treap_impl& other)
   {
      tree_type::swap(other);
      //This can throw
      using std::swap;
      swap(this->priv_pcomp(), other.priv_pcomp());
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const treap_impl &src, Cloner cloner, Disposer disposer)
   {
      tree_type::clone_from(src, cloner, disposer);
      this->priv_pcomp() = src.priv_pcomp();
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container before the upper bound.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->value_comp(), &this->get_real_value_traits());
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         key_node_pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      iterator ret(node_algorithms::insert_equal_upper_bound
         (this->tree_type::header_ptr(), to_insert, key_node_comp, key_node_pcomp), this->real_value_traits_ptr());
      this->tree_type::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator.
   //!
   //! <b>Effects</b>: Inserts x into the container, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case)
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(const_iterator hint, reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->value_comp(), &this->get_real_value_traits());
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         key_node_pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      iterator ret (node_algorithms::insert_equal
         (this->tree_type::header_ptr(), hint.pointed_node(), to_insert, key_node_comp, key_node_pcomp), this->real_value_traits_ptr());
      this->tree_type::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Inserts a each element of a range into the container
   //!   before the upper bound of the key of each element.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      iterator iend(this->end());
      for (; b != e; ++b)
         this->insert_equal(iend, *b);
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container if the value
   //!   is not already present.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert_unique(reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(value, this->value_comp(), this->priv_pcomp(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<iterator, bool> (insert_unique_commit(value, commit_data), true);
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator
   //!
   //! <b>Effects</b>: Tries to insert x into the container, using "hint" as a hint
   //!   to where it will be inserted.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time (two comparisons in the worst case)
   //!   if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_unique(const_iterator hint, reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(hint, value, this->value_comp(), this->priv_pcomp(), commit_data);
      if(!ret.second)
         return ret.first;
      return insert_unique_commit(value, commit_data);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Tries to insert each element of a range into the container.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //!
   //! <b>Throws</b>: If the internal value_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      if(this->empty()){
         iterator iend(this->end());
         for (; b != e; ++b)
            this->insert_unique(iend, *b);
      }
      else{
         for (; b != e; ++b)
            this->insert_unique(*b);
      }
   }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces
   //!   the same strict weak ordering as value_compare.
   //!   key_value_pcomp must be a comparison function that induces
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   key_value_pcomp and key_value_comp compare an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the key_value_comp or key_value_pcomp
   //!   ordering functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that
   //!   part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare, class KeyValuePrioCompare>
   std::pair<iterator, bool> insert_unique_check
      ( const KeyType &key, KeyValueCompare key_value_comp
      , KeyValuePrioCompare key_value_pcomp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         ocomp(key_value_comp, &this->get_real_value_traits());
      detail::key_nodeptr_comp<KeyValuePrioCompare, real_value_traits>
         pcomp(key_value_pcomp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->tree_type::header_ptr(), key, ocomp, pcomp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces
   //!   the same strict weak ordering as value_compare.
   //!   key_value_pcomp must be a comparison function that induces
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   key_value_pcomp and key_value_comp compare an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint"
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the key_value_comp or key_value_pcomp
   //!   ordering functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   constructing that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that key
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This can give a total
   //!   constant-time complexity to the insertion: check(O(1)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare, class KeyValuePrioCompare>
   std::pair<iterator, bool> insert_unique_check
      ( const_iterator hint, const KeyType &key
      , KeyValueCompare key_value_comp
      , KeyValuePrioCompare key_value_pcomp
      , insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         ocomp(key_value_comp, &this->get_real_value_traits());
      detail::key_nodeptr_comp<KeyValuePrioCompare, real_value_traits>
         pcomp(key_value_pcomp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->tree_type::header_ptr(), hint.pointed_node(), key, ocomp, pcomp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the container between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //! <b>Effects</b>: Inserts the value in the avl_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      node_algorithms::insert_unique_commit(this->tree_type::header_ptr(), to_insert, commit_data);
      this->tree_type::sz_traits().increment();
      return iterator(to_insert, this->real_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate
   //!
   //! <b>Effects</b>: Inserts x into the container before "pos".
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" container ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   iterator insert_before(const_iterator pos, reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      iterator ret (node_algorithms::insert_before
         (this->tree_type::header_ptr(), pos.pointed_node(), to_insert, pcomp), this->real_value_traits_ptr());
      this->tree_type::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the last position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      node_algorithms::push_back(this->tree_type::header_ptr(), to_insert, pcomp);
      this->tree_type::sz_traits().increment();
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the first position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      node_algorithms::push_front(this->tree_type::header_ptr(), to_insert, pcomp);
      this->tree_type::sz_traits().increment();
   }

   //! <b>Effects</b>: Erases the element pointed to by pos.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {
      const_iterator ret(i);
      ++ret;
      node_ptr to_erase(i.pointed_node());
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!node_algorithms::unique(to_erase));
      detail::key_nodeptr_comp<priority_compare, real_value_traits>
         key_node_pcomp(this->priv_pcomp(), &this->get_real_value_traits());
      node_algorithms::erase(this->tree_type::header_ptr(), to_erase, key_node_pcomp);
      this->tree_type::sz_traits().decrement();
      if(safemode_or_autounlink)
         node_algorithms::init(to_erase);
      return ret.unconst();
   }

   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  size_type n;   return private_erase(b, e, n);   }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return this->erase(value, this->value_comp());   }

   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws.
   //!   Equivalent guarantee to <i>while(beg != end) erase(beg++);</i>
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {
      std::pair<iterator,iterator> p = this->equal_range(key, comp);
      size_type n;
      private_erase(p.first, p.second, n);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by pos.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer)
   {
      node_ptr to_erase(i.pointed_node());
      iterator ret(this->erase(i));
      disposer(this->get_real_value_traits().to_value_ptr(to_erase));
      return ret;
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   iterator erase_and_dispose(iterator i, Disposer disposer)
   {  return this->erase_and_dispose(const_iterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  size_type n;   return private_erase(b, e, n, disposer);   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the priority_compare function throws then weak guarantee and heap invariants are broken.
   //!   The safest thing would be to clear or destroy the container.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {
      std::pair<iterator,iterator> p = this->equal_range(value);
      size_type n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the priority_compare function throws then weak guarantee and heap invariants are broken.
   //!   The safest thing would be to clear or destroy the container.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {
      std::pair<iterator,iterator> p = this->equal_range(key, comp);
      size_type n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Effects</b>: Erases all of the elements.
   //!
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear()
   {  tree_type::clear(); }

   //! <b>Effects</b>: Erases all of the elements calling disposer(p) for
   //!   each node to be erased.
   //! <b>Complexity</b>: Average complexity for is at most O(log(size() + N)),
   //!   where N is the number of elements in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. Calls N times to disposer functor.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer)
   {
      node_algorithms::clear_and_dispose(this->tree_type::header_ptr()
         , detail::node_disposer<Disposer, real_value_traits, TreapAlgorithms>(disposer, &this->get_real_value_traits()));
      node_algorithms::init_header(this->tree_type::header_ptr());
      this->tree_type::sz_traits().set_size(0);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::count(const_reference)const
   size_type count(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const;
   
   //! @copydoc ::boost::intrusive::bstree::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::bstree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::bstree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::bstree::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::bstree::remove_node
   void remove_node(reference value);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   /// @cond
   private:
   template<class Disposer>
   iterator private_erase(const_iterator b, const_iterator e, size_type &n, Disposer disposer)
   {
      for(n = 0; b != e; ++n)
        this->erase_and_dispose(b++, disposer);
      return b.unconst();
   }

   iterator private_erase(const_iterator b, const_iterator e, size_type &n)
   {
      for(n = 0; b != e; ++n)
        this->erase(b++);
      return b.unconst();
   }
   /// @endcond
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator< (const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator==(const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator!= (const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const treap_impl<T, Options...> &x, const treap_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(treap_impl<T, Options...> &x, treap_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c treap that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void>
#endif
struct make_treap
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

   typedef treap_impl
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
class treap
   :  public make_treap<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_treap
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::priority_compare   priority_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::real_value_traits  real_value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;
   typedef typename Base::reverse_iterator           reverse_iterator;
   typedef typename Base::const_reverse_iterator     const_reverse_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename real_value_traits::value_type, T>::value));

   explicit treap( const value_compare &cmp = value_compare()
                 , const priority_compare &pcmp = priority_compare()
                 , const value_traits &v_traits = value_traits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   treap( bool unique, Iterator b, Iterator e
       , const value_compare &cmp = value_compare()
       , const priority_compare &pcmp = priority_compare()
       , const value_traits &v_traits = value_traits())
      :  Base(unique, b, e, cmp, pcmp, v_traits)
   {}

   treap(BOOST_RV_REF(treap) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   treap& operator=(BOOST_RV_REF(treap) x)
   {  return static_cast<treap&>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static treap &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<treap &>(Base::container_from_end_iterator(end_iterator));   }

   static const treap &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const treap &>(Base::container_from_end_iterator(end_iterator));   }

   static treap &container_from_iterator(iterator it)
   {  return static_cast<treap &>(Base::container_from_iterator(it));   }

   static const treap &container_from_iterator(const_iterator it)
   {  return static_cast<const treap &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREAP_HPP
