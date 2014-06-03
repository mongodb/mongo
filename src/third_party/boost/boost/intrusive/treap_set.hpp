/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2009
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
template<class Config>
#endif
class treap_set_impl
{
   /// @cond
   typedef treap_impl<Config> tree_type;
   //! This class is
   //! movable
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

   static const bool constant_time_size = Config::constant_time_size;

   /// @cond
   private:
   tree_type tree_;
   /// @endcond

   public:
   //! <b>Effects</b>: Constructs an empty treap_set. 
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare object throws. 
   treap_set_impl( const value_compare &cmp      = value_compare()
                , const priority_compare &pcmp  = priority_compare()
                , const value_traits &v_traits  = value_traits()) 
      :  tree_(cmp, pcmp, v_traits)
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
      : tree_(true, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!   
   treap_set_impl(BOOST_RV_REF(treap_set_impl) x) 
      :  tree_(::boost::move(x.tree_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   treap_set_impl& operator=(BOOST_RV_REF(treap_set_impl) x) 
   {  tree_ = ::boost::move(x.tree_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the treap_set 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   ~treap_set_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return tree_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return tree_.end();  }

   //! <b>Effects</b>: Returns an iterator pointing to the highest priority object of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator top()
   { return tree_.top();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the tree..
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator top() const
   {  return this->ctop();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the tree..
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator ctop() const
   { return tree_.ctop();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return tree_.cend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin()
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const
   { return tree_.crbegin();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend()
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const
   { return tree_.crend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the highest priority object of the
   //!    reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rtop()
   {  return tree_.rtop();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority objec
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rtop() const
   {  return tree_.crtop();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority object
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crtop() const
   {  return tree_.crtop();  }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of treap_set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the treap_set associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static treap_set_impl &container_from_end_iterator(iterator end_iterator)
   {
      return *detail::parent_from_member<treap_set_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &treap_set_impl::tree_);
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of treap_set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the treap_set associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const treap_set_impl &container_from_end_iterator(const_iterator end_iterator)
   {
      return *detail::parent_from_member<treap_set_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &treap_set_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid iterator of set.
   //! 
   //! <b>Effects</b>: Returns a reference to the set associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static treap_set_impl &container_from_iterator(iterator it)
   {
      return *detail::parent_from_member<treap_set_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &treap_set_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid const_iterator of set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the set associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static const treap_set_impl &container_from_iterator(const_iterator it)
   {
      return *detail::parent_from_member<treap_set_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &treap_set_impl::tree_);
   }

   //! <b>Effects</b>: Returns the key_compare object used by the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_compare copy-constructor throws.
   key_compare key_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the value_compare object used by the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the priority_compare object used by the treap_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If priority_compare copy-constructor throws.
   priority_compare priority_comp() const
   {  return tree_.priority_comp();   }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return tree_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the treap_set.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if,
   //!   constant-time size option is enabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return tree_.size(); }

   //! <b>Effects</b>: Swaps the contents of two sets.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the swap() call for the comparison functor
   //!   found using ADL throws. Strong guarantee.
   void swap(treap_set_impl& other)
   { tree_.swap(other.tree_); }

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
   void clone_from(const treap_set_impl &src, Cloner cloner, Disposer disposer)
   {  tree_.clone_from(src.tree_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Tries to inserts value into the treap_set.
   //!
   //! <b>Returns</b>: If the value
   //!   is not already present inserts it and returns a pair containing the
   //!   iterator to the new value and true. If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false.
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare or priority_compare ordering function throw.
   //!   Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert(reference value)
   {  return tree_.insert_unique(value);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Tries to to insert x into the treap_set, using "hint" 
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: An iterator that points to the position where the 
   //!   new element was inserted into the treap_set.
   //! 
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: If the internal value_compare or priority_compare ordering
   //!   functions throw. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(const_iterator hint, reference value)
   {  return tree_.insert_unique(hint, value);  }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare.
   //!   key_value_pcomp must be a comparison function that induces 
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   key_value_pcomp and key_value_comp compare an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the treap_set, using
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
   //! <b>Throws</b>: If key_value_comp or key_value_pcomp ordering function throw.
   //!    Strong guarantee.
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
   //!   objects are inserted or erased from the treap_set.
   template<class KeyType, class KeyValueCompare, class KeyValuePriorityCompare>
   std::pair<iterator, bool> insert_check
      ( const KeyType &key, KeyValueCompare key_value_comp, KeyValuePriorityCompare key_value_pcomp
      , insert_commit_data &commit_data)
   {  return tree_.insert_unique_check(key, key_value_comp, key_value_pcomp, commit_data); }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare.
   //!   key_value_pcomp must be a comparison function that induces 
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   key_value_pcomp and key_value_comp compare an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the treap_set, using
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
   //! <b>Throws</b>: If key_value_comp or key_value_pcomp ordering function throw.
   //!    Strong guarantee.
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
   //!   objects are inserted or erased from the treap_set.
   template<class KeyType, class KeyValueCompare, class KeyValuePriorityCompare>
   std::pair<iterator, bool> insert_check
      ( const_iterator hint, const KeyType &key
      , KeyValueCompare key_value_comp, KeyValuePriorityCompare key_value_pcomp
      , insert_commit_data &commit_data)
   {  return tree_.insert_unique_check(hint, key, key_value_comp, key_value_pcomp, commit_data); }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the treap_set between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //! 
   //! <b>Effects</b>: Inserts the value in the treap_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //! 
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   iterator insert_commit(reference value, const insert_commit_data &commit_data)
   {  return tree_.insert_unique_commit(value, commit_data); }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Inserts a range into the treap_set.
   //! 
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: If the internal value_compare or priority_compare ordering function
   //!   throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_.insert_unique(b, e);  }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate. "value" must not be equal to any
   //!   inserted key according to the predicate.
   //!
   //! <b>Effects</b>: Inserts x into the treap before "pos".
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" treap ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   iterator insert_before(const_iterator pos, reference value)
   {  return tree_.insert_before(pos, value);  }

   //! <b>Requires</b>: value must be an lvalue, and it must be greater than
   //!   any inserted key according to the predicate.
   //!
   //! <b>Effects</b>: Inserts x into the treap in the last position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key treap ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(reference value)
   {  tree_.push_back(value);  }

   //! <b>Requires</b>: value must be an lvalue, and it must be less
   //!   than any inserted key according to the predicate.
   //!
   //! <b>Effects</b>: Inserts x into the treap in the first position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key treap ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(reference value)
   {  tree_.push_front(value);  }

   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {  return tree_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is at most 
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  return tree_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size()) + this->count(value)).
   //! 
   //! <b>Throws</b>: If internal value_compare or priority_compare
   //!   ordering functions throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return tree_.erase(value);  }

   //! <b>Effects</b>: Erases all the elements that compare equal with
   //!   the given key and the given comparison functor.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp or internal priority_compare
   //!   ordering functions throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {  return tree_.erase(key, comp);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //!   Disposer::operator()(pointer) is called for the removed element.
   //! 
   //! <b>Complexity</b>: Average complexity for erase element is constant time. 
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators 
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer)
   {  return tree_.erase_and_dispose(i, disposer);  }

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
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  return tree_.erase_and_dispose(b, e, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(value)). Basic guarantee.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {  return tree_.erase_and_dispose(value, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp or internal priority_compare ordering functions throw.
   //!   Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {  return tree_.erase_and_dispose(key, comp, disposer);  }

   //! <b>Effects</b>: Erases all the elements of the container.
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear()
   {  return tree_.clear();  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //! 
   //! <b>Effects</b>: Erases all the elements of the container.
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer)
   {  return tree_.clear_and_dispose(disposer);  }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count(const_reference value) const
   {  return tree_.find(value) != end();  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.find(key, comp) != end();  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator lower_bound(const_reference value)
   {  return tree_.lower_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key according to the comparison functor is not less than k or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //! 
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp)
   {  return tree_.lower_bound(key, comp);  }

   //! <b>Effects</b>: Returns a const iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator lower_bound(const_reference value) const
   {  return tree_.lower_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the first element whose
   //!   key according to the comparison functor is not less than k or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //! 
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.lower_bound(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator upper_bound(const_reference value)
   {  return tree_.upper_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key according to the comparison functor is greater than key or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp)
   {  return tree_.upper_bound(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator upper_bound(const_reference value) const
   {  return tree_.upper_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the first element whose
   //!   key according to the comparison functor is greater than key or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.upper_bound(key, comp);  }

   //! <b>Effects</b>: Finds an iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator find(const_reference value)
   {  return tree_.find(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the comparison functor or end() if that element 
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp)
   {  return tree_.find(key, comp);  }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator find(const_reference value) const
   {  return tree_.find(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   "key" according to the comparison functor or end() if that element 
   //!   does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.find(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return tree_.equal_range(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a range containing all elements whose key is k 
   //!   according to the comparison functor or an empty range 
   //!   that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp)
   {  return tree_.equal_range(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return tree_.equal_range(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a range containing all elements whose key is k 
   //!   according to the comparison functor or an empty range 
   //!   that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.equal_range(key, comp);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the treap_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static iterator s_iterator_to(reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   treap_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the treap_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value)
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   treap_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value shall not be in a treap_set/treap_multiset.
   //! 
   //! <b>Effects</b>: init_node puts the hook of a value in a well-known default
   //!   state.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Note</b>: This function puts the hook in the well-known default state
   //!   used by auto_unlink and safe hooks.
   static void init_node(reference value)
   { tree_type::init_node(value);   }

   //! <b>Effects</b>: Unlinks the leftmost node from the tree.
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function breaks the tree and the tree can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the tree.
   pointer unlink_leftmost_without_rebalance()
   {  return tree_.unlink_leftmost_without_rebalance();  }

   //! <b>Requires</b>: replace_this must be a valid iterator of *this
   //!   and with_this must not be inserted in any tree.
   //! 
   //! <b>Effects</b>: Replaces replace_this in its position in the
   //!   tree with with_this. The tree does not need to be rebalanced.
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   with_this is not equivalent to *replace_this according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   void replace_node(iterator replace_this, reference with_this)
   {  tree_.replace_node(replace_this, with_this);   }

   //! <b>Effects</b>: Rebalances the tree.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear.
   void rebalance()
   {  tree_.rebalance(); }

   //! <b>Requires</b>: old_root is a node of a tree.
   //! 
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   iterator rebalance_subtree(iterator root)
   {  return tree_.rebalance_subtree(root); }

   //! <b>Returns</b>: The balance factor (alpha) used in this tree
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   float balance_factor() const
   {  return tree_.balance_factor(); }

   //! <b>Requires</b>: new_alpha must be a value between 0.5 and 1.0
   //! 
   //! <b>Effects</b>: Establishes a new balance factor (alpha) and rebalances
   //!   the tree if the new balance factor is stricter (less) than the old factor.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   void balance_factor(float new_alpha)
   {  tree_.balance_factor(new_alpha); }

   /// @cond
   friend bool operator==(const treap_set_impl &x, const treap_set_impl &y)
   {  return x.tree_ == y.tree_;  }

   friend bool operator<(const treap_set_impl &x, const treap_set_impl &y)
   {  return x.tree_ < y.tree_;  }
   /// @endcond
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator!=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_set_impl<T, Options...> &x, const treap_set_impl<T, Options...> &y)
#else
(const treap_set_impl<Config> &x, const treap_set_impl<Config> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_set_impl<T, Options...> &x, const treap_set_impl<T, Options...> &y)
#else
(const treap_set_impl<Config> &x, const treap_set_impl<Config> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_set_impl<T, Options...> &x, const treap_set_impl<T, Options...> &y)
#else
(const treap_set_impl<Config> &x, const treap_set_impl<Config> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_set_impl<T, Options...> &x, const treap_set_impl<T, Options...> &y)
#else
(const treap_set_impl<Config> &x, const treap_set_impl<Config> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(treap_set_impl<T, Options...> &x, treap_set_impl<T, Options...> &y)
#else
(treap_set_impl<Config> &x, treap_set_impl<Config> &y)
#endif
{  x.swap(y);  }

//! Helper metafunction to define a \c treap_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#endif
struct make_treap_set
{
   /// @cond
   typedef treap_set_impl
      < typename make_treap_opt<T, 
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
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

   treap_set( const value_compare &cmp    = value_compare()
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
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }

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
template<class Config>
#endif
class treap_multiset_impl
{
   /// @cond
   typedef treap_impl<Config> tree_type;

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

   static const bool constant_time_size = Config::constant_time_size;

   /// @cond
   private:
   tree_type tree_;
   /// @endcond

   public:
   //! <b>Effects</b>: Constructs an empty treap_multiset. 
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare/priority_compare objects throw. 
   treap_multiset_impl( const value_compare &cmp     = value_compare()
                     , const priority_compare &pcmp = priority_compare()
                     , const value_traits &v_traits = value_traits()) 
      :  tree_(cmp, pcmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type. 
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //! 
   //! <b>Effects</b>: Constructs an empty treap_multiset and inserts elements from 
   //!   [b, e).
   //! 
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is the distance between first and last
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare/priority_compare objects throw. 
   template<class Iterator>
   treap_multiset_impl( Iterator b, Iterator e
                , const value_compare &cmp     = value_compare()
                , const priority_compare &pcmp = priority_compare()
                , const value_traits &v_traits = value_traits())
      : tree_(false, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!   
   treap_multiset_impl(BOOST_RV_REF(treap_multiset_impl) x) 
      :  tree_(::boost::move(x.tree_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   treap_multiset_impl& operator=(BOOST_RV_REF(treap_multiset_impl) x) 
   {  tree_ = ::boost::move(x.tree_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the treap_multiset 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   ~treap_multiset_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return tree_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return tree_.cend();  }

   //! <b>Effects</b>: Returns an iterator pointing to the highest priority object of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator top()
   { return tree_.top();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the tree..
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator top() const
   {  return this->ctop();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the tree..
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator ctop() const
   { return tree_.ctop();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin()
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const
   { return tree_.crbegin();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend()
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const
   { return tree_.crend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the highest priority object of the
   //!    reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rtop()
   {  return tree_.rtop();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority objec
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rtop() const
   {  return tree_.crtop();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority object
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crtop() const
   {  return tree_.crtop();  }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of treap_multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the treap_multiset associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static treap_multiset_impl &container_from_end_iterator(iterator end_iterator)
   {
      return *detail::parent_from_member<treap_multiset_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &treap_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of treap_multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the treap_multiset associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const treap_multiset_impl &container_from_end_iterator(const_iterator end_iterator)
   {
      return *detail::parent_from_member<treap_multiset_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &treap_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid iterator of multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the multiset associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static treap_multiset_impl &container_from_iterator(iterator it)
   {
      return *detail::parent_from_member<treap_multiset_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &treap_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid const_iterator of multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the multiset associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const treap_multiset_impl &container_from_iterator(const_iterator it)
   {
      return *detail::parent_from_member<treap_multiset_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &treap_multiset_impl::tree_);
   }

   //! <b>Effects</b>: Returns the key_compare object used by the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_compare copy-constructor throws.
   key_compare key_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the value_compare object used by the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the priority_compare object used by the treap_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If priority_compare copy-constructor throws.
   priority_compare priority_comp() const
   {  return tree_.priority_comp();   }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return tree_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the treap_multiset.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if,
   //!   constant-time size option is enabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return tree_.size(); }

   //! <b>Effects</b>: Swaps the contents of two treap_multisets.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the swap() call for the comparison functor
   //!   found using ADL throws. Strong guarantee.
   void swap(treap_multiset_impl& other)
   { tree_.swap(other.tree_); }

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
   void clone_from(const treap_multiset_impl &src, Cloner cloner, Disposer disposer)
   {  tree_.clone_from(src.tree_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts value into the treap_multiset.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare or priority_compare ordering
   //!  function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(reference value)
   {  return tree_.insert_equal(value);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts x into the treap_multiset, using pos as a hint to
   //!   where it will be inserted.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: If internal value_compare or priority_compare ordering functions throw.
   //!   Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(const_iterator hint, reference value)
   {  return tree_.insert_equal(hint, value);  }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Inserts a range into the treap_multiset.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: If internal value_compare or priority_compare ordering functions throw.
   //!   Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_.insert_equal(b, e);  }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate
   //!
   //! <b>Effects</b>: Inserts x into the treap before "pos".
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" treap ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   iterator insert_before(const_iterator pos, reference value)
   {  return tree_.insert_before(pos, value);  }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key.
   //!
   //! <b>Effects</b>: Inserts x into the treap in the last position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key treap ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(reference value)
   {  tree_.push_back(value);  }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the treap in the first position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key treap ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(reference value)
   {  tree_.push_front(value);  }

   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //! 
   //! <b>Complexity</b>: Average complexity is constant time. 
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {  return tree_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //!
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is at most 
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  return tree_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(value)).
   //! 
   //! <b>Throws</b>: If the internal value_compare or priority_compare ordering
   //!   functiona throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return tree_.erase(value);  }

   //! <b>Effects</b>: Erases all the elements that compare equal with
   //!   the given key and the given comparison functor.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp or internal priority_compare ordering functions throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {  return tree_.erase(key, comp);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //!
   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //!   Disposer::operator()(pointer) is called for the removed element.
   //! 
   //! <b>Complexity</b>: Average complexity for erase element is constant time. 
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators 
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer)
   {  return tree_.erase_and_dispose(i, disposer);  }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   iterator erase_and_dispose(iterator i, Disposer disposer)
   {  return this->erase_and_dispose(const_iterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is at most 
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: If the internal priority_compare function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  return tree_.erase_and_dispose(b, e, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(value)).
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {  return tree_.erase_and_dispose(value, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp or internal priority_compare ordering functions throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {  return tree_.erase_and_dispose(key, comp, disposer);  }

   //! <b>Effects</b>: Erases all the elements of the container.
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear()
   {  return tree_.clear();  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //! 
   //! <b>Effects</b>: Erases all the elements of the container.
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer)
   {  return tree_.clear_and_dispose(disposer);  }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count(const_reference value) const
   {  return tree_.count(value);  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.count(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator lower_bound(const_reference value)
   {  return tree_.lower_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key according to the comparison functor is not less than k or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //! 
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp)
   {  return tree_.lower_bound(key, comp);  }

   //! <b>Effects</b>: Returns a const iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator lower_bound(const_reference value) const
   {  return tree_.lower_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the first element whose
   //!   key according to the comparison functor is not less than k or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //! 
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.lower_bound(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator upper_bound(const_reference value)
   {  return tree_.upper_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key according to the comparison functor is greater than key or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp)
   {  return tree_.upper_bound(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator upper_bound(const_reference value) const
   {  return tree_.upper_bound(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the first element whose
   //!   key according to the comparison functor is greater than key or 
   //!   end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.upper_bound(key, comp);  }

   //! <b>Effects</b>: Finds an iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   iterator find(const_reference value)
   {  return tree_.find(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the comparison functor or end() if that element 
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp)
   {  return tree_.find(key, comp);  }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   const_iterator find(const_reference value) const
   {  return tree_.find(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   "key" according to the comparison functor or end() if that element 
   //!   does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.find(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return tree_.equal_range(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a range containing all elements whose key is k 
   //!   according to the comparison functor or an empty range 
   //!   that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp)
   {  return tree_.equal_range(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return tree_.equal_range(value);  }

   //! <b>Requires</b>: comp must imply the same element order as
   //!   value_compare. Usually key is the part of the value_type
   //!   that is used in the ordering functor.
   //!
   //! <b>Effects</b>: Finds a range containing all elements whose key is k 
   //!   according to the comparison functor or an empty range 
   //!   that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.equal_range(key, comp);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the treap_multiset
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static iterator s_iterator_to(reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   treap_multiset that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the treap_multiset
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value)
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a treap_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   treap_multiset that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value shall not be in a treap_multiset/treap_multiset.
   //! 
   //! <b>Effects</b>: init_node puts the hook of a value in a well-known default
   //!   state.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Note</b>: This function puts the hook in the well-known default state
   //!   used by auto_unlink and safe hooks.
   static void init_node(reference value)
   { tree_type::init_node(value);   }

   //! <b>Effects</b>: Unlinks the leftmost node from the tree.
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function breaks the tree and the tree can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the tree.
   pointer unlink_leftmost_without_rebalance()
   {  return tree_.unlink_leftmost_without_rebalance();  }

   //! <b>Requires</b>: replace_this must be a valid iterator of *this
   //!   and with_this must not be inserted in any tree.
   //! 
   //! <b>Effects</b>: Replaces replace_this in its position in the
   //!   tree with with_this. The tree does not need to be rebalanced.
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   with_this is not equivalent to *replace_this according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   void replace_node(iterator replace_this, reference with_this)
   {  tree_.replace_node(replace_this, with_this);   }

   //! <b>Effects</b>: Rebalances the tree.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear.
   void rebalance()
   {  tree_.rebalance(); }

   //! <b>Requires</b>: old_root is a node of a tree.
   //! 
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   iterator rebalance_subtree(iterator root)
   {  return tree_.rebalance_subtree(root); }

   //! <b>Returns</b>: The balance factor (alpha) used in this tree
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   float balance_factor() const
   {  return tree_.balance_factor(); }

   //! <b>Requires</b>: new_alpha must be a value between 0.5 and 1.0
   //! 
   //! <b>Effects</b>: Establishes a new balance factor (alpha) and rebalances
   //!   the tree if the new balance factor is stricter (less) than the old factor.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   void balance_factor(float new_alpha)
   {  tree_.balance_factor(new_alpha); }

   /// @cond
   friend bool operator==(const treap_multiset_impl &x, const treap_multiset_impl &y)
   {  return x.tree_ == y.tree_;  }

   friend bool operator<(const treap_multiset_impl &x, const treap_multiset_impl &y)
   {  return x.tree_ < y.tree_;  }
   /// @endcond
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator!=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_multiset_impl<T, Options...> &x, const treap_multiset_impl<T, Options...> &y)
#else
(const treap_multiset_impl<Config> &x, const treap_multiset_impl<Config> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_multiset_impl<T, Options...> &x, const treap_multiset_impl<T, Options...> &y)
#else
(const treap_multiset_impl<Config> &x, const treap_multiset_impl<Config> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_multiset_impl<T, Options...> &x, const treap_multiset_impl<T, Options...> &y)
#else
(const treap_multiset_impl<Config> &x, const treap_multiset_impl<Config> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const treap_multiset_impl<T, Options...> &x, const treap_multiset_impl<T, Options...> &y)
#else
(const treap_multiset_impl<Config> &x, const treap_multiset_impl<Config> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(treap_multiset_impl<T, Options...> &x, treap_multiset_impl<T, Options...> &y)
#else
(treap_multiset_impl<Config> &x, treap_multiset_impl<Config> &y)
#endif
{  x.swap(y);  }

//! Helper metafunction to define a \c treap_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#endif
struct make_treap_multiset
{
   /// @cond
   typedef treap_multiset_impl
      < typename make_treap_opt<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
         >::type
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
   //Movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(treap_multiset)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::priority_compare   priority_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   treap_multiset( const value_compare &cmp = value_compare()
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
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }

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
