/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2009
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_UNORDERED_SET_HPP
#define BOOST_INTRUSIVE_UNORDERED_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/hashtable.hpp>
#include <boost/move/move.hpp>
#include <iterator>


namespace boost {
namespace intrusive {

//! The class template unordered_set is an intrusive container, that mimics most of 
//! the interface of std::tr1::unordered_set as described in the C++ TR1.
//!
//! unordered_set is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: unordered_set needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! unordered_set more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, \c power_2_buckets<> and \c cache_begin<>.
//!
//! unordered_set only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size unordered_sets because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant-time size unordered_sets are mainly provided to support auto-unlink hooks.
//!
//! unordered_set, unlike std::unordered_set, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehasing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
class unordered_set_impl
{
   /// @cond
   private:
   typedef hashtable_impl<Config> table_type;

   //! This class is
   //! movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(unordered_set_impl)

   typedef table_type implementation_defined;
   /// @endcond

   public:
   typedef typename implementation_defined::value_type                  value_type;
   typedef typename implementation_defined::value_traits                value_traits;
   typedef typename implementation_defined::bucket_traits               bucket_traits;
   typedef typename implementation_defined::pointer                     pointer;
   typedef typename implementation_defined::const_pointer               const_pointer;
   typedef typename implementation_defined::reference                   reference;
   typedef typename implementation_defined::const_reference             const_reference;
   typedef typename implementation_defined::difference_type             difference_type;
   typedef typename implementation_defined::size_type                   size_type;
   typedef typename implementation_defined::key_type                    key_type;
   typedef typename implementation_defined::key_equal                   key_equal;
   typedef typename implementation_defined::hasher                      hasher;
   typedef typename implementation_defined::bucket_type                 bucket_type;
   typedef typename implementation_defined::bucket_ptr                  bucket_ptr;
   typedef typename implementation_defined::iterator                    iterator;
   typedef typename implementation_defined::const_iterator              const_iterator;
   typedef typename implementation_defined::insert_commit_data          insert_commit_data;
   typedef typename implementation_defined::local_iterator              local_iterator;
   typedef typename implementation_defined::const_local_iterator        const_local_iterator;
   typedef typename implementation_defined::node_traits                 node_traits;
   typedef typename implementation_defined::node                        node;
   typedef typename implementation_defined::node_ptr                    node_ptr;
   typedef typename implementation_defined::const_node_ptr              const_node_ptr;
   typedef typename implementation_defined::node_algorithms             node_algorithms;

   /// @cond
   private:
   table_type table_;
   /// @endcond

   public:

   //! <b>Requires</b>: buckets must not be being used by any other resource.
   //!
   //! <b>Effects</b>: Constructs an empty unordered_set_impl, storing a reference
   //!   to the bucket array and copies of the hasher and equal functors.
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of Hash or Equal throws. 
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed. 
   unordered_set_impl( const bucket_traits &b_traits
                     , const hasher & hash_func = hasher()
                     , const key_equal &equal_func = key_equal()
                     , const value_traits &v_traits = value_traits()) 
      :  table_(b_traits, hash_func, equal_func, v_traits)
   {}

   //! <b>Requires</b>: buckets must not be being used by any other resource
   //!   and Dereferencing iterator must yield an lvalue of type value_type.
   //! 
   //! <b>Effects</b>: Constructs an empty unordered_set and inserts elements from 
   //!   [b, e).
   //!   
   //! <b>Complexity</b>: If N is std::distance(b, e): Average case is O(N)
   //!   (with a good hash function and with buckets_len >= N),worst case O(N2).
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of hasher or key_equal throws. 
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed. 
   template<class Iterator>
   unordered_set_impl( Iterator b
                     , Iterator e
                     , const bucket_traits &b_traits
                     , const hasher & hash_func = hasher()
                     , const key_equal &equal_func = key_equal()
                     , const value_traits &v_traits = value_traits()) 
      :  table_(b_traits, hash_func, equal_func, v_traits)
   {  table_.insert_unique(b, e);  }

   //! <b>Effects</b>: to-do
   //!   
   unordered_set_impl(BOOST_RV_REF(unordered_set_impl) x) 
      :  table_(::boost::move(x.table_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   unordered_set_impl& operator=(BOOST_RV_REF(unordered_set_impl) x) 
   {  table_ = ::boost::move(x.table_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the unordered_set 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements in the unordered_set, if 
   //!   it's a safe-mode or auto-unlink value. Otherwise constant.
   //! 
   //! <b>Throws</b>: Nothing.
   ~unordered_set_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return table_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return table_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return table_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return table_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return table_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return table_.cend();  }

   //! <b>Effects</b>: Returns the hasher object used by the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If hasher copy-constructor throws.
   hasher hash_function() const
   { return table_.hash_function(); }

   //! <b>Effects</b>: Returns the key_equal object used by the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_equal copy-constructor throws.
   key_equal key_eq() const
   { return table_.key_eq(); }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: if constant-time size and cache_last options are disabled,
   //!   average constant time (worst case, with empty() == true: O(this->bucket_count()).
   //!   Otherwise constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return table_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the unordered_set.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if
   //!   constant-time size option is disabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return table_.size(); }

   //! <b>Requires</b>: the hasher and the equality function unqualified swap
   //!   call should not throw.
   //! 
   //! <b>Effects</b>: Swaps the contents of two unordered_sets.
   //!   Swaps also the contained bucket array and equality and hasher functors.
   //! 
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the swap() call for the comparison or hash functors
   //!   found using ADL throw. Basic guarantee.
   void swap(unordered_set_impl& other)
   { table_.swap(other.table_); }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes that compare equal and produce the same
   //!   hash than the original node.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the 
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. The hash function and the equality
   //!   predicate are copied from the source.
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!
   //!   If any operation throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!   
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //! 
   //! <b>Throws</b>: If cloner or hasher throw or hash or equality predicate copying
   //!   throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const unordered_set_impl &src, Cloner cloner, Disposer disposer)
   {  table_.clone_from(src.table_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Tries to inserts value into the unordered_set.
   //!
   //! <b>Returns</b>: If the value
   //!   is not already present inserts it and returns a pair containing the
   //!   iterator to the new value and true. If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert(reference value)
   {  return table_.insert_unique(value);  }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Equivalent to this->insert(t) for each element in [b, e).
   //! 
   //! <b>Complexity</b>: Average case O(N), where N is std::distance(b, e).
   //!   Worst case O(N*this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  table_.insert_unique(b, e);  }

   //! <b>Requires</b>: "hasher" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hasher" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the unordered_set, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hasher or key_value_equal throw. Strong guarantee.
   //! 
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the hash or the equality is much cheaper to
   //!   construct than the value_type and this function offers the possibility to
   //!   use that the part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time.
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the unordered_set.
   //!
   //!   After a successful rehashing insert_commit_data remains valid.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<iterator, bool> insert_check
      (const KeyType &key, KeyHasher hasher, KeyValueEqual key_value_equal, insert_commit_data &commit_data)
   {  return table_.insert_unique_check(key, hasher, key_value_equal, commit_data); }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the unordered_set between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //! 
   //! <b>Effects</b>: Inserts the value in the unordered_set using the information obtained
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
   //!
   //!   After a successful rehashing insert_commit_data remains valid.
   iterator insert_commit(reference value, const insert_commit_data &commit_data)
   {  return table_.insert_unique_commit(value, commit_data); }

   //! <b>Effects</b>: Erases the element pointed to by i. 
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased element. No destructors are called.
   void erase(const_iterator i)
   {  table_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //! 
   //! <b>Complexity</b>: Average case O(std::distance(b, e)),
   //!   worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void erase(const_iterator b, const_iterator e)
   {  table_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.  Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return table_.erase(value);  }

   //! <b>Requires</b>: "hasher" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hasher" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Erases all the elements that have the same hash and
   //!   compare equal with the given key.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   size_type erase(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.erase(key, hash_func, equal_func);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by i. 
   //!   Disposer::operator()(pointer) is called for the removed element.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators 
   //!    to the erased elements.
   template<class Disposer>
   void erase_and_dispose(const_iterator i, Disposer disposer
                              /// @cond
                              , typename detail::enable_if_c<!detail::is_convertible<Disposer, const_iterator>::value >::type * = 0
                              /// @endcond
                              )
   {  table_.erase_and_dispose(i, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Complexity</b>: Average case O(std::distance(b, e)),
   //!   worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   void erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  table_.erase_and_dispose(b, e, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {  return table_.erase_and_dispose(value, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "equal_func".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyHasher, class KeyValueEqual, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func, Disposer disposer)
   {  return table_.erase_and_dispose(key, hash_func, equal_func, disposer);  }

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
   {  return table_.clear();  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //! 
   //! <b>Effects</b>: Erases all of the elements. 
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
   {  return table_.clear_and_dispose(disposer);  }

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   size_type count(const_reference value) const
   {  return table_.find(value) != end();  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   size_type count(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.find(key, hash_func, equal_func) != end();  }

   //! <b>Effects</b>: Finds an iterator to the first element is equal to
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   iterator find(const_reference value)
   {  return table_.find(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the given hasher and equality functor or end() if
   //!   that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   iterator find(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.find(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   "key" or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   const_iterator find(const_reference value) const
   {  return table_.find(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the given hasher and equality functor or end() if
   //!   that element does not exist.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   const_iterator find(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.find(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return table_.equal_range(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, hash_func)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or the equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.equal_range(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return table_.equal_range(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, equal_func)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the hash_func or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.equal_range(key, hash_func, equal_func);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator belonging to the unordered_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the internal hash function throws.
   iterator iterator_to(reference value)
   {  return table_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator belonging to the
   //!   unordered_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the internal hash function throws.
   const_iterator iterator_to(const_reference value) const
   {  return table_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static local_iterator s_local_iterator_to(reference value)
   {  return table_type::s_local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_local_iterator s_local_iterator_to(const_reference value)
   {  return table_type::s_local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   local_iterator local_iterator_to(reference value)
   {  return table_.local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_local_iterator local_iterator_to(const_reference value) const
   {  return table_.local_iterator_to(value);  }

   //! <b>Effects</b>: Returns the number of buckets passed in the constructor
   //!   or the last rehash function.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type bucket_count() const
   {  return table_.bucket_count();   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns the number of elements in the nth bucket.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type bucket_size(size_type n) const
   {  return table_.bucket_size(n);   }

   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the hash functor throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   size_type bucket(const value_type& k) const
   {  return table_.bucket(k);   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If hash_func throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   template<class KeyType, class KeyHasher>
   size_type bucket(const KeyType& k,  KeyHasher hash_func) const
   {  return table_.bucket(k, hash_func);   }

   //! <b>Effects</b>: Returns the bucket array pointer passed in the constructor
   //!   or the last rehash function.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bucket_ptr bucket_pointer() const
   {  return table_.bucket_pointer();   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   local_iterator begin(size_type n)
   {  return table_.begin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator begin(size_type n) const
   {  return table_.begin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator cbegin(size_type n) const
   {  return table_.cbegin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   local_iterator end(size_type n)
   {  return table_.end(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator end(size_type n) const
   {  return table_.end(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator cend(size_type n) const
   {  return table_.cend(n);   }

   //! <b>Requires</b>: new_buckets must be a pointer to a new bucket array
   //!   or the same as the old bucket array. new_size is the length of the
   //!   the array pointed by new_buckets. If new_buckets == this->bucket_pointer()
   //!   n can be bigger or smaller than this->bucket_count().
   //!
   //! <b>Effects</b>: Updates the internal reference with the new bucket erases
   //!   the values from the old bucket and inserts then in the new one. 
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //! 
   //! <b>Complexity</b>: Average case linear in this->size(), worst case quadratic.
   //! 
   //! <b>Throws</b>: If the hasher functor throws. Basic guarantee.
   void rehash(const bucket_traits &new_bucket_traits)
   {  table_.rehash(new_bucket_traits); }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>: 
   //! 
   //! <b>Complexity</b>: 
   //! 
   //! <b>Throws</b>:
   //!
   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(bool grow = true)
   {  return table_.incremental_rehash(grow);  }

   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(const bucket_traits &new_bucket_traits)
   {  return table_.incremental_rehash(new_bucket_traits);  }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>: 
   //! 
   //! <b>Complexity</b>: 
   //! 
   //! <b>Throws</b>: 
   size_type split_count() const
   {  return table_.split_count(); }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is bigger than n. This suggestion can be used
   //!   to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the 
   //!   higher possible value is returned.
   //! 
   //! <b>Complexity</b>: Amortized constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static size_type suggested_upper_bucket_count(size_type n)
   {  return table_type::suggested_upper_bucket_count(n);  }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is smaller than n. This suggestion can be used
   //!   to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the 
   //!   lower possible value is returned.
   //! 
   //! <b>Complexity</b>: Amortized constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static size_type suggested_lower_bucket_count(size_type n)
   {  return table_type::suggested_lower_bucket_count(n);  }
};

//! Helper metafunction to define an \c unordered_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none
                , class O5 = none, class O6 = none
                , class O7 = none, class O8 = none
                , class O9 = none, class O10= none
                >
#endif
struct make_unordered_set
{
   /// @cond
   typedef unordered_set_impl
      <  typename make_hashtable_opt
            <T, true, 
               #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
               O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
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
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10>
#else
template<class T, class ...Options>
#endif
class unordered_set
   :  public make_unordered_set<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type
{
   typedef typename make_unordered_set
      <T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type   Base;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(unordered_set)

   public:
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::bucket_traits      bucket_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;
   typedef typename Base::bucket_ptr         bucket_ptr;
   typedef typename Base::size_type          size_type;
   typedef typename Base::hasher             hasher;
   typedef typename Base::key_equal          key_equal;

   unordered_set  ( const bucket_traits &b_traits
                  , const hasher & hash_func = hasher()
                  , const key_equal &equal_func = key_equal()
                  , const value_traits &v_traits = value_traits()) 
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   template<class Iterator>
   unordered_set  ( Iterator b
                  , Iterator e
                  , const bucket_traits &b_traits
                  , const hasher & hash_func = hasher()
                  , const key_equal &equal_func = key_equal()
                  , const value_traits &v_traits = value_traits()) 
      :  Base(b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   unordered_set(BOOST_RV_REF(unordered_set) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   unordered_set& operator=(BOOST_RV_REF(unordered_set) x)
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }
};

#endif


//! The class template unordered_multiset is an intrusive container, that mimics most of 
//! the interface of std::tr1::unordered_multiset as described in the C++ TR1.
//!
//! unordered_multiset is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: unordered_multiset needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! unordered_multiset more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, \c power_2_buckets<> and \c cache_begin<>.
//!
//! unordered_multiset only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size unordered_multisets because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant-time size unordered_multisets are mainly provided to support auto-unlink hooks.
//!
//! unordered_multiset, unlike std::unordered_set, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehasing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
class unordered_multiset_impl
{
   /// @cond
   private:
   typedef hashtable_impl<Config> table_type;
   /// @endcond

   //Movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(unordered_multiset_impl)

   typedef table_type implementation_defined;

   public:
   typedef typename implementation_defined::value_type                  value_type;
   typedef typename implementation_defined::value_traits                value_traits;
   typedef typename implementation_defined::bucket_traits               bucket_traits;
   typedef typename implementation_defined::pointer                     pointer;
   typedef typename implementation_defined::const_pointer               const_pointer;
   typedef typename implementation_defined::reference                   reference;
   typedef typename implementation_defined::const_reference             const_reference;
   typedef typename implementation_defined::difference_type             difference_type;
   typedef typename implementation_defined::size_type                   size_type;
   typedef typename implementation_defined::key_type                    key_type;
   typedef typename implementation_defined::key_equal                   key_equal;
   typedef typename implementation_defined::hasher                      hasher;
   typedef typename implementation_defined::bucket_type                 bucket_type;
   typedef typename implementation_defined::bucket_ptr                  bucket_ptr;
   typedef typename implementation_defined::iterator                    iterator;
   typedef typename implementation_defined::const_iterator              const_iterator;
   typedef typename implementation_defined::insert_commit_data          insert_commit_data;
   typedef typename implementation_defined::local_iterator              local_iterator;
   typedef typename implementation_defined::const_local_iterator        const_local_iterator;
   typedef typename implementation_defined::node_traits                 node_traits;
   typedef typename implementation_defined::node                        node;
   typedef typename implementation_defined::node_ptr                    node_ptr;
   typedef typename implementation_defined::const_node_ptr              const_node_ptr;
   typedef typename implementation_defined::node_algorithms             node_algorithms;

   /// @cond
   private:
   table_type table_;
   /// @endcond

   public:

   //! <b>Requires</b>: buckets must not be being used by any other resource.
   //!
   //! <b>Effects</b>: Constructs an empty unordered_multiset, storing a reference
   //!   to the bucket array and copies of the hasher and equal functors.
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of Hash or Equal throws. 
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed. 
   unordered_multiset_impl ( const bucket_traits &b_traits
                           , const hasher & hash_func = hasher()
                           , const key_equal &equal_func = key_equal()
                           , const value_traits &v_traits = value_traits()) 
      :  table_(b_traits, hash_func, equal_func, v_traits)
   {}

   //! <b>Requires</b>: buckets must not be being used by any other resource
   //!   and Dereferencing iterator must yield an lvalue of type value_type.
   //! 
   //! <b>Effects</b>: Constructs an empty unordered_multiset and inserts elements from 
   //!   [b, e).
   //!   
   //! <b>Complexity</b>: If N is std::distance(b, e): Average case is O(N)
   //!   (with a good hash function and with buckets_len >= N),worst case O(N2).
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of hasher or key_equal throws. 
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed.
   template<class Iterator>
   unordered_multiset_impl ( Iterator b
                           , Iterator e
                           , const bucket_traits &b_traits
                           , const hasher & hash_func = hasher()
                           , const key_equal &equal_func = key_equal()
                           , const value_traits &v_traits = value_traits()) 
      :  table_(b_traits, hash_func, equal_func, v_traits)
   {  table_.insert_equal(b, e);  }

   //! <b>Effects</b>: to-do
   //!   
   unordered_multiset_impl(BOOST_RV_REF(unordered_multiset_impl) x) 
      :  table_(::boost::move(x.table_))
   {}

   //! <b>Effects</b>: to-do
   //!   
   unordered_multiset_impl& operator=(BOOST_RV_REF(unordered_multiset_impl) x) 
   {  table_ = ::boost::move(x.table_);  return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the unordered_multiset 
   //!   are not deleted (i.e. no destructors are called).
   //! 
   //! <b>Complexity</b>: Linear to the number of elements in the unordered_multiset, if 
   //!   it's a safe-mode or auto-unlink value. Otherwise constant.
   //! 
   //! <b>Throws</b>: Nothing.
   ~unordered_multiset_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   { return table_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_multiset.
   //!
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   { return table_.begin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_multiset.
   //!
   //! <b>Complexity</b>: Constant time if `cache_begin<>` is true. Amortized
   //!   constant time with worst case (empty unordered_set) O(this->bucket_count())
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   { return table_.cbegin();  }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   { return table_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   { return table_.end();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   { return table_.cend();  }

   //! <b>Effects</b>: Returns the hasher object used by the unordered_set.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If hasher copy-constructor throws.
   hasher hash_function() const
   { return table_.hash_function(); }

   //! <b>Effects</b>: Returns the key_equal object used by the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If key_equal copy-constructor throws.
   key_equal key_eq() const
   { return table_.key_eq(); }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: if constant-time size and cache_last options are disabled,
   //!   average constant time (worst case, with empty() == true: O(this->bucket_count()).
   //!   Otherwise constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   { return table_.empty(); }

   //! <b>Effects</b>: Returns the number of elements stored in the unordered_multiset.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this if
   //!   constant-time size option is disabled. Constant-time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   { return table_.size(); }

   //! <b>Requires</b>: the hasher and the equality function unqualified swap
   //!   call should not throw.
   //! 
   //! <b>Effects</b>: Swaps the contents of two unordered_multisets.
   //!   Swaps also the contained bucket array and equality and hasher functors.
   //!
   //! 
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the swap() call for the comparison or hash functors
   //!   found using ADL throw. Basic guarantee.
   void swap(unordered_multiset_impl& other)
   { table_.swap(other.table_); }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes that compare equal and produce the same
   //!   hash than the original node.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the 
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. The hash function and the equality
   //!   predicate are copied from the source.
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!
   //!   If any operation throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!   
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //! 
   //! <b>Throws</b>: If cloner or hasher throw or hash or equality predicate copying
   //!   throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const unordered_multiset_impl &src, Cloner cloner, Disposer disposer)
   {  table_.clone_from(src.table_, cloner, disposer);  }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts value into the unordered_multiset.
   //!
   //! <b>Returns</b>: An iterator to the new inserted value.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert(reference value)
   {  return table_.insert_equal(value);  }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Equivalent to this->insert(t) for each element in [b, e).
   //! 
   //! <b>Complexity</b>: Average case is O(N), where N is the
   //!   size of the range.
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  table_.insert_equal(b, e);  }

   //! <b>Effects</b>: Erases the element pointed to by i. 
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased element. No destructors are called.
   void erase(const_iterator i)
   {  table_.erase(i);  }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //! 
   //! <b>Complexity</b>: Average case O(std::distance(b, e)),
   //!   worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void erase(const_iterator b, const_iterator e)
   {  table_.erase(b, e);  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return table_.erase(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Erases all the elements that have the same hash and
   //!   compare equal with the given key.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the hash_func or the equal_func functors throws.
   //!   Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   size_type erase(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.erase(key, hash_func, equal_func);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by i. 
   //!   Disposer::operator()(pointer) is called for the removed element.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators 
   //!    to the erased elements.
   template<class Disposer>
   void erase_and_dispose(const_iterator i, Disposer disposer
                              /// @cond
                              , typename detail::enable_if_c<!detail::is_convertible<Disposer, const_iterator>::value >::type * = 0
                              /// @endcond
                              )
   {  table_.erase_and_dispose(i, disposer);  }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   void erase_and_dispose(const_iterator i, Disposer disposer)
   {  this->erase_and_dispose(const_iterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Complexity</b>: Average case O(std::distance(b, e)),
   //!   worst case O(this->size()).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   void erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  table_.erase_and_dispose(b, e, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {  return table_.erase_and_dispose(value, disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "equal_func".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If hash_func or equal_func throw. Basic guarantee.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyHasher, class KeyValueEqual, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func, Disposer disposer)
   {  return table_.erase_and_dispose(key, hash_func, equal_func, disposer);  }

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
   {  return table_.clear();  }

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
   {  return table_.clear_and_dispose(disposer);  }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   size_type count(const_reference value) const
   {  return table_.count(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   size_type count(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.count(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Finds an iterator to the first element whose value is 
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   iterator find(const_reference value)
   {  return table_.find(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the given hasher and equality functor or end() if
   //!   that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   iterator find(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.find(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   "key" or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   const_iterator find(const_reference value) const
   {  return table_.find(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   "key" according to the given hasher and equality functor or end() if
   //!   that element does not exist.
   //! 
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   const_iterator find(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.find(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return table_.equal_range(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, equal_func)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<iterator,iterator> equal_range
      (const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func)
   {  return table_.equal_range(key, hash_func, equal_func);  }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return table_.equal_range(value);  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "key_value_equal" must be a equality function that induces 
   //!   the same equality as key_equal. The difference is that
   //!   "key_value_equal" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such 
   //!   elements exist.
   //! 
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, equal_func)).
   //!   Worst case O(this->size()).
   //! 
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {  return table_.equal_range(key, hash_func, equal_func);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator belonging to the unordered_multiset
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the hash function throws.
   iterator iterator_to(reference value)
   {  return table_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_multiset of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator belonging to the
   //!   unordered_multiset that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the hash function throws.
   const_iterator iterator_to(const_reference value) const
   {  return table_.iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static local_iterator s_local_iterator_to(reference value)
   {  return table_type::s_local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_local_iterator s_local_iterator_to(const_reference value)
   {  return table_type::s_local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   local_iterator local_iterator_to(reference value)
   {  return table_.local_iterator_to(value);  }

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_local_iterator local_iterator_to(const_reference value) const
   {  return table_.local_iterator_to(value);  }

   //! <b>Effects</b>: Returns the number of buckets passed in the constructor
   //!   or the last rehash function.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type bucket_count() const
   {  return table_.bucket_count();   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns the number of elements in the nth bucket.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type bucket_size(size_type n) const
   {  return table_.bucket_size(n);   }

   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the hash functor throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   size_type bucket(const value_type& k) const
   {  return table_.bucket(k);   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces 
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the hash functor throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   template<class KeyType, class KeyHasher>
   size_type bucket(const KeyType& k, const KeyHasher &hash_func) const
   {  return table_.bucket(k, hash_func);   }

   //! <b>Effects</b>: Returns the bucket array pointer passed in the constructor
   //!   or the last rehash function.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bucket_ptr bucket_pointer() const
   {  return table_.bucket_pointer();   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   local_iterator begin(size_type n)
   {  return table_.begin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator begin(size_type n) const
   {  return table_.begin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator cbegin(size_type n) const
   {  return table_.cbegin(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   local_iterator end(size_type n)
   {  return table_.end(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator end(size_type n) const
   {  return table_.end(n);   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket. 
   const_local_iterator cend(size_type n) const
   {  return table_.cend(n);   }

   //! <b>Requires</b>: new_buckets must be a pointer to a new bucket array
   //!   or the same as the old bucket array. new_size is the length of the
   //!   the array pointed by new_buckets. If new_buckets == this->bucket_pointer()
   //!   n can be bigger or smaller than this->bucket_count().
   //!
   //! <b>Effects</b>: Updates the internal reference with the new bucket erases
   //!   the values from the old bucket and inserts then in the new one. 
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //! 
   //! <b>Complexity</b>: Average case linear in this->size(), worst case quadratic.
   //! 
   //! <b>Throws</b>: If the hasher functor throws.
   void rehash(const bucket_traits &new_bucket_traits)
   {  table_.rehash(new_bucket_traits); }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>: 
   //! 
   //! <b>Complexity</b>: 
   //! 
   //! <b>Throws</b>:
   //!
   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(bool grow = true)
   {  return table_.incremental_rehash(grow);  }

   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(const bucket_traits &new_bucket_traits)
   {  return table_.incremental_rehash(new_bucket_traits);  }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>: 
   //! 
   //! <b>Complexity</b>: 
   //! 
   //! <b>Throws</b>: 
   size_type split_count() const
   {  return table_.split_count(); }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is bigger than n. This suggestion can be used
   //!   to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the 
   //!   higher possible value is returned.
   //! 
   //! <b>Complexity</b>: Amortized constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static size_type suggested_upper_bucket_count(size_type n)
   {  return table_type::suggested_upper_bucket_count(n);  }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is smaller than n. This suggestion can be used
   //!   to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the 
   //!   lower possible value is returned.
   //! 
   //! <b>Complexity</b>: Amortized constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   static size_type suggested_lower_bucket_count(size_type n)
   {  return table_type::suggested_lower_bucket_count(n);  }
};

//! Helper metafunction to define an \c unordered_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none
                , class O5 = none, class O6 = none
                , class O7 = none, class O8 = none
                , class O9 = none, class O10= none
                >
#endif
struct make_unordered_multiset
{
   /// @cond
   typedef unordered_multiset_impl
      <  typename make_hashtable_opt
            <T, false, 
               #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
               O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
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
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10>
#else
template<class T, class ...Options>
#endif
class unordered_multiset
   :  public make_unordered_multiset<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type
{
   typedef typename make_unordered_multiset
      <T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type   Base;
   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(unordered_multiset)

   public:
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::bucket_traits      bucket_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;
   typedef typename Base::bucket_ptr         bucket_ptr;
   typedef typename Base::size_type          size_type;
   typedef typename Base::hasher             hasher;
   typedef typename Base::key_equal          key_equal;

   unordered_multiset( const bucket_traits &b_traits
                     , const hasher & hash_func = hasher()
                     , const key_equal &equal_func = key_equal()
                     , const value_traits &v_traits = value_traits()) 
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   template<class Iterator>
   unordered_multiset( Iterator b
                     , Iterator e
                     , const bucket_traits &b_traits
                     , const hasher & hash_func = hasher()
                     , const key_equal &equal_func = key_equal()
                     , const value_traits &v_traits = value_traits()) 
      :  Base(b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   unordered_multiset(BOOST_RV_REF(unordered_multiset) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   unordered_multiset& operator=(BOOST_RV_REF(unordered_multiset) x)
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }
};

#endif

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_UNORDERED_SET_HPP
