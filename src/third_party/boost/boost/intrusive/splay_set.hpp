/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2009
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_SPLAY_SET_HPP
#define BOOST_INTRUSIVE_SPLAY_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/splaytree.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/move/move.hpp>
#include <iterator>

namespace boost {
namespace intrusive {

//! The class template splay_set is an intrusive container, that mimics most of 
//! the interface of std::set as described in the C++ standard.
//! 
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
class splay_set_impl
{
   /// @cond
   typedef splaytree_impl<Config> tree_type;
   //! This class is
   //! movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(splay_set_impl)

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

   static const bool constant_time_size = Config::constant_time_size;

   /// @cond
   private:
   tree_type tree_;
   /// @endcond

   public:
   //! <b>Effects</b>: Constructs an empty splay_set. 
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare object throws. 
   splay_set_impl( const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits()) 
      :  tree_(cmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type. 
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //! 
   //! <b>Effects</b>: Constructs an empty splay_set and inserts elements from 
   //!   [b, e).
   //! 
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using 
   //!   comp and otherwise amortized N * log N, where N is std::distance(last, first).
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws. 
   template<class Iterator>
   splay_set_impl( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      : tree_(true, b, e, cmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!   
   splay_set_impl(BOOST_RV_REF(splay_set_impl) x) 
      :  tree_(::boost::move(x.tree_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   splay_set_impl& operator=(BOOST_RV_REF(splay_set_impl) x) 
   {  tree_ = ::boost::move(x.tree_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the splay_set 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   ~splay_set_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return tree_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return tree_.cend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin()
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const
   { return tree_.crbegin();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend()
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const
   { return tree_.crend();  }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of splay_set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the splay_set associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static splay_set_impl &container_from_end_iterator(iterator end_iterator)
   {
      return *detail::parent_from_member<splay_set_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &splay_set_impl::tree_);
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of splay_set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the splay_set associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const splay_set_impl &container_from_end_iterator(const_iterator end_iterator)
   {
      return *detail::parent_from_member<splay_set_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &splay_set_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid iterator of set.
   //! 
   //! <b>Effects</b>: Returns a reference to the set associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static splay_set_impl &container_from_iterator(iterator it)
   {
      return *detail::parent_from_member<splay_set_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &splay_set_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid const_iterator of set.
   //! 
   //! <b>Effects</b>: Returns a const reference to the set associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static const splay_set_impl &container_from_iterator(const_iterator it)
   {
      return *detail::parent_from_member<splay_set_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &splay_set_impl::tree_);
   }

   //! <b>Effects</b>: Returns the key_compare object used by the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_compare copy-constructor throws.
   key_compare key_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the value_compare object used by the splay_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return tree_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the splay_set.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if,
   //!   constant-time size option is enabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return tree_.size(); }

   //! <b>Effects</b>: Swaps the contents of two splay_sets.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the swap() call for the comparison functor
   //!   found using ADL throws. Strong guarantee.
   void swap(splay_set_impl& other)
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
   void clone_from(const splay_set_impl &src, Cloner cloner, Disposer disposer)
   {  tree_.clone_from(src.tree_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Tries to inserts value into the splay_set.
   //!
   //! <b>Returns</b>: If the value
   //!   is not already present inserts it and returns a pair containing the
   //!   iterator to the new value and true. If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert(reference value)
   {  return tree_.insert_unique(value);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Tries to to insert x into the splay_set, using "hint" 
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: An iterator that points to the position where the 
   //!   new element was inserted into the splay_set.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(const_iterator hint, reference value)
   {  return tree_.insert_unique(hint, value);  }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the splay_set, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //!
   //! <b>Throws</b>: If the key_value_comp ordering function throws. Strong guarantee.
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
   //!   objects are inserted or erased from the splay_set.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {  return tree_.insert_unique_check(key, key_value_comp, commit_data); }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the splay_set, using
   //!   a user provided key instead of the value itself, using "hint" 
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the key_value_comp ordering function throws. Strong guarantee.
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
   //!   objects are inserted or erased from the splay_set.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {  return tree_.insert_unique_check(hint, key, key_value_comp, commit_data); }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the splay_set between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //! 
   //! <b>Effects</b>: Inserts the value in the splay_set using the information obtained
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
   //! <b>Effects</b>: Inserts a range into the splay_set.
   //! 
   //! <b>Complexity</b>: Insert range is amortized O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_.insert_unique(b, e);  }

   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {  return tree_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is amortized
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  return tree_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Amortized O(log(size()) + this->count(value)).
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Basic guarantee.
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If the comp ordering function throws. Basic guarantee.
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
   //! <b>Throws</b>: Nothing.
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
   //! <b>Throws</b>: Nothing.
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(value)). Basic guarantee.
   //! 
   //! <b>Throws</b>: Nothing.
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp ordering function throws. Basic guarantee.
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
   //! <b>Complexity</b>: Amortized logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count(const_reference value)
   {  return tree_.find(value) != end();  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp)
   {  return tree_.find(key, comp) != end();  }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count_dont_splay(const_reference value)const
   {  return tree_.find_dont_splay(value) != end();  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count_dont_splay(const KeyType& key, KeyValueCompare comp)const
   {  return tree_.find_dont_splay(key, comp) != end();  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator lower_bound_dont_splay(const_reference value) const
   {  return tree_.lower_bound_dont_splay(value);  }

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
   const_iterator lower_bound_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.lower_bound_dont_splay(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator upper_bound_dont_splay(const_reference value) const
   {  return tree_.upper_bound_dont_splay(value);  }

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
   const_iterator upper_bound_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.upper_bound_dont_splay(key, comp);  }

   //! <b>Effects</b>: Finds an iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator find_dont_splay(const_reference value) const
   {  return tree_.find_dont_splay(value);  }

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
   const_iterator find_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.find_dont_splay(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
      equal_range_dont_splay(const_reference value) const
   {  return tree_.equal_range_dont_splay(value);  }

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
      equal_range_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.equal_range_dont_splay(key, comp);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a splay_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the splay_set
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

   //! <b>Requires</b>: value must be an lvalue and shall be in a splay_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   splay_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a splay_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the splay_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value)
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a splay_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   splay_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value shall not be in a splay_set/multisplay_set.
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

   //! <b>Requires</b>: i must be a valid iterator of *this.
   //! 
   //! <b>Effects</b>: Rearranges the splay set so that the element pointed by i
   //!   is placed as the root of the tree, improving future searches of this value.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   void splay_up(iterator i)
   {  tree_.splay_up(i);   }

   //! <b>Effects</b>: Rearranges the splay set so that if *this stores an element
   //!   with a key equivalent to value the element is placed as the root of the
   //!   tree. If the element is not present returns the last node compared with the key.
   //!   If the tree is empty, end() is returned.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Returns</b>: An iterator to the new root of the tree, end() if the tree is empty.
   //!
   //! <b>Throws</b>: If the comparison functor throws.
   template<class KeyType, class KeyNodePtrCompare>
   iterator splay_down(const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_.splay_down(key, comp);   }

   //! <b>Effects</b>: Rearranges the splay set so that if *this stores an element
   //!   with a key equivalent to value the element is placed as the root of the
   //!   tree.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Returns</b>: An iterator to the new root of the tree, end() if the tree is empty.
   //!
   //! <b>Throws</b>: If the predicate throws.
   iterator splay_down(const value_type &value)
   {  return tree_.splay_down(value);   }

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

   /// @cond
   friend bool operator==(const splay_set_impl &x, const splay_set_impl &y)
   {  return x.tree_ == y.tree_;  }

   friend bool operator<(const splay_set_impl &x, const splay_set_impl &y)
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
(const splay_set_impl<T, Options...> &x, const splay_set_impl<T, Options...> &y)
#else
(const splay_set_impl<Config> &x, const splay_set_impl<Config> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_set_impl<T, Options...> &x, const splay_set_impl<T, Options...> &y)
#else
(const splay_set_impl<Config> &x, const splay_set_impl<Config> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_set_impl<T, Options...> &x, const splay_set_impl<T, Options...> &y)
#else
(const splay_set_impl<Config> &x, const splay_set_impl<Config> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_set_impl<T, Options...> &x, const splay_set_impl<T, Options...> &y)
#else
(const splay_set_impl<Config> &x, const splay_set_impl<Config> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(splay_set_impl<T, Options...> &x, splay_set_impl<T, Options...> &y)
#else
(splay_set_impl<Config> &x, splay_set_impl<Config> &y)
#endif
{  x.swap(y);  }

//! Helper metafunction to define a \c splay_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#endif
struct make_splay_set
{
   /// @cond
   typedef splay_set_impl
      < typename make_splaytree_opt<T, 
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
class splay_set
   :  public make_splay_set<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type
{
   typedef typename make_splay_set
      <T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(splay_set)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   splay_set( const value_compare &cmp = value_compare()
         , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   splay_set( Iterator b, Iterator e
      , const value_compare &cmp = value_compare()
      , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, v_traits)
   {}

   splay_set(BOOST_RV_REF(splay_set) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   splay_set& operator=(BOOST_RV_REF(splay_set) x)
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }

   static splay_set &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<splay_set &>(Base::container_from_end_iterator(end_iterator));   }

   static const splay_set &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const splay_set &>(Base::container_from_end_iterator(end_iterator));   }

   static splay_set &container_from_iterator(iterator it)
   {  return static_cast<splay_set &>(Base::container_from_iterator(it));   }

   static const splay_set &container_from_iterator(const_iterator it)
   {  return static_cast<const splay_set &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template splay_multiset is an intrusive container, that mimics most of 
//! the interface of std::multiset as described in the C++ standard.
//! 
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
class splay_multiset_impl
{
   /// @cond
   typedef splaytree_impl<Config> tree_type;

   //Movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(splay_multiset_impl)
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

   static const bool constant_time_size = Config::constant_time_size;

   /// @cond
   private:
   tree_type tree_;
   /// @endcond

   public:
   //! <b>Effects</b>: Constructs an empty splay_multiset. 
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws. 
   splay_multiset_impl( const value_compare &cmp = value_compare()
                , const value_traits &v_traits = value_traits()) 
      :  tree_(cmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type. 
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //! 
   //! <b>Effects</b>: Constructs an empty splay_multiset and inserts elements from 
   //!   [b, e).
   //! 
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise amortized N * log N, where N is the distance between first and last.
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws. 
   template<class Iterator>
   splay_multiset_impl( Iterator b, Iterator e
                , const value_compare &cmp = value_compare()
                , const value_traits &v_traits = value_traits())
      : tree_(false, b, e, cmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!   
   splay_multiset_impl(BOOST_RV_REF(splay_multiset_impl) x) 
      :  tree_(::boost::move(x.tree_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   splay_multiset_impl& operator=(BOOST_RV_REF(splay_multiset_impl) x) 
   {  tree_ = ::boost::move(x.tree_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the set 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   ~splay_multiset_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return tree_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return tree_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return tree_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return tree_.cend();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin()
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const
   { return tree_.rbegin();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const
   { return tree_.crbegin();  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend()
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const
   { return tree_.rend();  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const
   { return tree_.crend();  }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of splay_multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the splay_multiset associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static splay_multiset_impl &container_from_end_iterator(iterator end_iterator)
   {
      return *detail::parent_from_member<splay_multiset_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &splay_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of splay_multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the splay_multiset associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const splay_multiset_impl &container_from_end_iterator(const_iterator end_iterator)
   {
      return *detail::parent_from_member<splay_multiset_impl, tree_type>
         ( &tree_type::container_from_end_iterator(end_iterator)
         , &splay_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid iterator of multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the multiset associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static splay_multiset_impl &container_from_iterator(iterator it)
   {
      return *detail::parent_from_member<splay_multiset_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &splay_multiset_impl::tree_);
   }

   //! <b>Precondition</b>: it must be a valid const_iterator of multiset.
   //! 
   //! <b>Effects</b>: Returns a const reference to the multiset associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const splay_multiset_impl &container_from_iterator(const_iterator it)
   {
      return *detail::parent_from_member<splay_multiset_impl, tree_type>
         ( &tree_type::container_from_iterator(it)
         , &splay_multiset_impl::tree_);
   }

   //! <b>Effects</b>: Returns the key_compare object used by the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_compare copy-constructor throws.
   key_compare key_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns the value_compare object used by the splay_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const
   { return tree_.value_comp(); }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return tree_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the splay_multiset.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if,
   //!   constant-time size option is enabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return tree_.size(); }

   //! <b>Effects</b>: Swaps the contents of two splay_multisets.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the swap() call for the comparison functor
   //!   found using ADL throws. Strong guarantee.
   void swap(splay_multiset_impl& other)
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
   void clone_from(const splay_multiset_impl &src, Cloner cloner, Disposer disposer)
   {  tree_.clone_from(src.tree_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts value into the splay_multiset.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(reference value)
   {  return tree_.insert_equal(this->end(), value);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts x into the splay_multiset, using pos as a hint to
   //!   where it will be inserted.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(const_iterator hint, reference value)
   {  return tree_.insert_equal(hint, value);  }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Inserts a range into the splay_multiset.
   //! 
   //! <b>Returns</b>: An iterator that points to the position where the new
   //!   element was inserted.
   //! 
   //! <b>Complexity</b>: Insert range is amortized O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  tree_.insert_equal(b, e);  }

   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //! 
   //! <b>Complexity</b>: Average complexity is constant time. 
   //! 
   //! <b>Returns</b>: An iterator to the element after the erased element.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {  return tree_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //!
   //! <b>Returns</b>: An iterator to the element after the erased elements.
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is amortized
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  return tree_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(value)).
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Basic guarantee.
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp ordering function throws. Basic guarantee.
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
   //! <b>Throws</b>: Nothing.
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
   //! <b>Complexity</b>: Average complexity for erase range is amortized
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: Nothing.
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(value)).
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
   //! <b>Complexity</b>: Amortized O(log(size() + this->count(key, comp)).
   //! 
   //! <b>Throws</b>: If comp ordering function throws. Basic guarantee.
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
   //! <b>Complexity</b>: Amortized logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count(const_reference value)
   {  return tree_.count(value);  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp)
   {  return tree_.count(key, comp);  }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws.
   size_type count_dont_splay(const_reference value) const
   {  return tree_.count_dont_splay(value);  }

   //! <b>Effects</b>: Returns the number of contained elements with the same key
   //!   compared with the given comparison functor.
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: If comp ordering function throws.
   template<class KeyType, class KeyValueCompare>
   size_type count_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.count_dont_splay(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator lower_bound_dont_splay(const_reference value) const
   {  return tree_.lower_bound_dont_splay(value);  }

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
   const_iterator lower_bound_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.lower_bound_dont_splay(key, comp);  }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator upper_bound_dont_splay(const_reference value) const
   {  return tree_.upper_bound_dont_splay(value);  }

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
   const_iterator upper_bound_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.upper_bound_dont_splay(key, comp);  }

   //! <b>Effects</b>: Finds an iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
   const_iterator find_dont_splay(const_reference value) const
   {  return tree_.find_dont_splay(value);  }

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
   const_iterator find_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.find_dont_splay(key, comp);  }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
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
   //! <b>Complexity</b>: Amortized logarithmic.
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
      equal_range_dont_splay(const_reference value) const
   {  return tree_.equal_range_dont_splay(value);  }

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
      equal_range_dont_splay(const KeyType& key, KeyValueCompare comp) const
   {  return tree_.equal_range_dont_splay(key, comp);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
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

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value)
   {  return tree_type::s_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value)
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const
   {  return tree_.iterator_to(value);  }

   //! <b>Requires</b>: value shall not be in a set/splay_multiset.
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

   //! <b>Requires</b>: i must be a valid iterator of *this.
   //! 
   //! <b>Effects</b>: Rearranges the splay set so that the element pointed by i
   //!   is placed as the root of the tree, improving future searches of this value.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   void splay_up(iterator i)
   {  tree_.splay_up(i);   }

   //! <b>Effects</b>: Rearranges the splay set so that if *this stores an element
   //!   with a key equivalent to value the element is placed as the root of the
   //!   tree. If the element is not present returns the last node compared with the key.
   //!   If the tree is empty, end() is returned.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Returns</b>: An iterator to the new root of the tree, end() if the tree is empty.
   //!
   //! <b>Throws</b>: If the comparison functor throws.
   template<class KeyType, class KeyNodePtrCompare>
   iterator splay_down(const KeyType &key, KeyNodePtrCompare comp)
   {  return tree_.splay_down(key, comp);   }

   //! <b>Effects</b>: Rearranges the splay set so that if *this stores an element
   //!   with a key equivalent to value the element is placed as the root of the
   //!   tree.
   //! 
   //! <b>Complexity</b>: Amortized logarithmic.
   //! 
   //! <b>Returns</b>: An iterator to the new root of the tree, end() if the tree is empty.
   //!
   //! <b>Throws</b>: If the predicate throws.
   iterator splay_down(const value_type &value)
   {  return tree_.splay_down(value);   }

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

   /// @cond
   friend bool operator==(const splay_multiset_impl &x, const splay_multiset_impl &y)
   {  return x.tree_ == y.tree_;  }

   friend bool operator<(const splay_multiset_impl &x, const splay_multiset_impl &y)
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
(const splay_multiset_impl<T, Options...> &x, const splay_multiset_impl<T, Options...> &y)
#else
(const splay_multiset_impl<Config> &x, const splay_multiset_impl<Config> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_multiset_impl<T, Options...> &x, const splay_multiset_impl<T, Options...> &y)
#else
(const splay_multiset_impl<Config> &x, const splay_multiset_impl<Config> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_multiset_impl<T, Options...> &x, const splay_multiset_impl<T, Options...> &y)
#else
(const splay_multiset_impl<Config> &x, const splay_multiset_impl<Config> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const splay_multiset_impl<T, Options...> &x, const splay_multiset_impl<T, Options...> &y)
#else
(const splay_multiset_impl<Config> &x, const splay_multiset_impl<Config> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(splay_multiset_impl<T, Options...> &x, splay_multiset_impl<T, Options...> &y)
#else
(splay_multiset_impl<Config> &x, splay_multiset_impl<Config> &y)
#endif
{  x.swap(y);  }

//! Helper metafunction to define a \c splay_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#endif
struct make_splay_multiset
{
   /// @cond
   typedef splay_multiset_impl
      < typename make_splaytree_opt<T, 
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
class splay_multiset
   :  public make_splay_multiset<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type
{
   typedef typename make_splay_multiset
      <T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(splay_multiset)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   splay_multiset( const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   splay_multiset( Iterator b, Iterator e
           , const value_compare &cmp = value_compare()
           , const value_traits &v_traits = value_traits())
      :  Base(b, e, cmp, v_traits)
   {}

   splay_multiset(BOOST_RV_REF(splay_multiset) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   splay_multiset& operator=(BOOST_RV_REF(splay_multiset) x)
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }

   static splay_multiset &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<splay_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static const splay_multiset &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const splay_multiset &>(Base::container_from_end_iterator(end_iterator));   }

   static splay_multiset &container_from_iterator(iterator it)
   {  return static_cast<splay_multiset &>(Base::container_from_iterator(it));   }

   static const splay_multiset &container_from_iterator(const_iterator it)
   {  return static_cast<const splay_multiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SPLAY_SET_HPP
