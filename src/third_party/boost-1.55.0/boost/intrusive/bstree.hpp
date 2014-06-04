/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2013-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_BSTREE_HPP
#define BOOST_INTRUSIVE_BSTREE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/set_hook.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/clear_on_destructor_base.hpp>
#include <boost/intrusive/detail/function_detector.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/move/move.hpp>

namespace boost {
namespace intrusive {

/// @cond

struct bstree_defaults
{
   typedef detail::default_bstree_hook proto_value_traits;
   static const bool constant_time_size = true;
   typedef std::size_t size_type;
   typedef void compare;
   static const bool floating_point = true;  //For sgtree
   typedef void priority;  //For treap
};

template<class ValueTraits, algo_types AlgoType>
struct bstbase3
   : public detail::get_real_value_traits<ValueTraits>::type::node_traits::node
   , public ValueTraits
{
   typedef ValueTraits                                               value_traits;
   typedef typename detail::get_real_value_traits<ValueTraits>::type real_value_traits;
   typedef typename real_value_traits::node_traits                   node_traits;
   typedef typename node_traits::node                                node_type;
   typedef typename get_algo<AlgoType, node_traits>::type            node_algorithms;
   typedef typename node_traits::node_ptr                            node_ptr;
   typedef typename node_traits::const_node_ptr                      const_node_ptr;

   bstbase3(const ValueTraits &vtraits)
      : ValueTraits(vtraits)
   {}

   static const bool external_value_traits =
      detail::external_value_traits_bool_is_true<ValueTraits>::value;

   node_ptr header_ptr()
   {  return pointer_traits<node_ptr>::pointer_to(static_cast<node_type&>(*this));  }

   const_node_ptr header_ptr() const
   {  return pointer_traits<const_node_ptr>::pointer_to(static_cast<const node_type&>(*this));  }

   const value_traits &val_traits() const
   {  return *this;  }

   value_traits &val_traits()
   {  return *this;  }

   const real_value_traits &get_real_value_traits(detail::bool_<false>) const
   {  return *this;  }

   const real_value_traits &get_real_value_traits(detail::bool_<true>) const
   {  return this->val_traits().get_value_traits(*this);  }

   real_value_traits &get_real_value_traits(detail::bool_<false>)
   {  return *this;  }

   real_value_traits &get_real_value_traits(detail::bool_<true>)
   {  return this->val_traits().get_value_traits(*this);  }

   const real_value_traits &get_real_value_traits() const
   {  return this->get_real_value_traits(detail::bool_<external_value_traits>());  }

   real_value_traits &get_real_value_traits()
   {  return this->get_real_value_traits(detail::bool_<external_value_traits>());  }

   typedef typename pointer_traits<node_ptr>::template rebind_pointer<const real_value_traits>::type const_real_value_traits_ptr;

   const_real_value_traits_ptr real_value_traits_ptr() const
   {  return pointer_traits<const_real_value_traits_ptr>::pointer_to(this->get_real_value_traits());  }


   typedef tree_iterator<real_value_traits, false> iterator;
   typedef tree_iterator<real_value_traits, true>  const_iterator;
   typedef boost::intrusive::detail::reverse_iterator<iterator>         reverse_iterator;
   typedef boost::intrusive::detail::reverse_iterator<const_iterator>   const_reverse_iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::pointer)                          pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::const_pointer)                    const_pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::element_type)               value_type;
   typedef BOOST_INTRUSIVE_IMPDEF(value_type)                                                   key_type;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::reference)                  reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::reference)            const_reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::difference_type)      difference_type;
   static const bool safemode_or_autounlink = is_safe_autounlink<real_value_traits::link_mode>::value;
   static const bool stateful_value_traits = detail::is_stateful_value_traits<real_value_traits>::value;

   iterator begin()
   {  return iterator (node_traits::get_left(this->header_ptr()), this->real_value_traits_ptr());   }

   const_iterator begin() const
   {  return cbegin();   }

   const_iterator cbegin() const
   {  return const_iterator (node_traits::get_left(this->header_ptr()), this->real_value_traits_ptr());   }

   iterator end()
   {  return iterator (this->header_ptr(), this->real_value_traits_ptr());  }

   const_iterator end() const
   {  return cend();  }

   const_iterator cend() const
   {  return const_iterator (detail::uncast(this->header_ptr()), this->real_value_traits_ptr());  }

   reverse_iterator rbegin()
   {  return reverse_iterator(end());  }

   const_reverse_iterator rbegin() const
   {  return const_reverse_iterator(end());  }

   const_reverse_iterator crbegin() const
   {  return const_reverse_iterator(end());  }

   reverse_iterator rend()
   {  return reverse_iterator(begin());   }

   const_reverse_iterator rend() const
   {  return const_reverse_iterator(begin());   }

   const_reverse_iterator crend() const
   {  return const_reverse_iterator(begin());   }

   void replace_node(iterator replace_this, reference with_this)
   {
      node_algorithms::replace_node( get_real_value_traits().to_node_ptr(*replace_this)
                                   , this->header_ptr()
                                   , get_real_value_traits().to_node_ptr(with_this));
      if(safemode_or_autounlink)
         node_algorithms::init(replace_this.pointed_node());
   }

   void rebalance()
   {  node_algorithms::rebalance(this->header_ptr()); }

   iterator rebalance_subtree(iterator root)
   {  return iterator(node_algorithms::rebalance_subtree(root.pointed_node()), this->real_value_traits_ptr()); }

   static iterator s_iterator_to(reference value)
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      return iterator (value_traits::to_node_ptr(value), const_real_value_traits_ptr());
   }

   static const_iterator s_iterator_to(const_reference value)
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      return const_iterator (value_traits::to_node_ptr(const_cast<reference> (value)), const_real_value_traits_ptr());
   }

   iterator iterator_to(reference value)
   {  return iterator (value_traits::to_node_ptr(value), this->real_value_traits_ptr()); }

   const_iterator iterator_to(const_reference value) const
   {  return const_iterator (value_traits::to_node_ptr(const_cast<reference> (value)), this->real_value_traits_ptr()); }

   static void init_node(reference value)
   { node_algorithms::init(value_traits::to_node_ptr(value)); }

};

template<class ValueTraits, class VoidOrKeyComp, algo_types AlgoType>
struct bstbase2
   : public bstbase3<ValueTraits, AlgoType>
   , public detail::ebo_functor_holder<typename get_less< VoidOrKeyComp
                            , typename detail::get_real_value_traits<ValueTraits>::type::value_type
                            >::type>
{
   typedef bstbase3<ValueTraits, AlgoType>                           treeheader_t;
   typedef typename treeheader_t::real_value_traits                  real_value_traits;
   typedef typename treeheader_t::node_algorithms                    node_algorithms;
   typedef typename get_less
      < VoidOrKeyComp, typename real_value_traits::value_type>::type value_compare;
   typedef BOOST_INTRUSIVE_IMPDEF(value_compare)                     key_compare;
   typedef typename treeheader_t::iterator                           iterator;
   typedef typename treeheader_t::const_iterator                     const_iterator;
   typedef typename treeheader_t::node_ptr                           node_ptr;
   typedef typename treeheader_t::const_node_ptr                     const_node_ptr;

   bstbase2(const value_compare &comp, const ValueTraits &vtraits)
      : treeheader_t(vtraits), detail::ebo_functor_holder<value_compare>(comp)
   {}

   const value_compare &comp() const
   {  return this->get();  }
   
   value_compare &comp()
   {  return this->get();  }

   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::pointer)                          pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::const_pointer)                    const_pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::element_type)               value_type;
   typedef BOOST_INTRUSIVE_IMPDEF(value_type)                                                   key_type;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::reference)                  reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::reference)            const_reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::difference_type)      difference_type;
   typedef typename node_algorithms::insert_commit_data insert_commit_data;

   value_compare value_comp() const
   {  return this->comp();   }

   key_compare key_comp() const
   {  return this->comp();   }

   iterator lower_bound(const_reference value)
   {  return this->lower_bound(value, this->comp());   }

   const_iterator lower_bound(const_reference value) const
   {  return this->lower_bound(value, this->comp());   }

   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return iterator(node_algorithms::lower_bound
         (this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return const_iterator(node_algorithms::lower_bound
         (this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   iterator upper_bound(const_reference value)
   {  return this->upper_bound(value, this->comp());   }

   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return iterator(node_algorithms::upper_bound
         (this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   const_iterator upper_bound(const_reference value) const
   {  return this->upper_bound(value, this->comp());   }

   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return const_iterator(node_algorithms::upper_bound
         (this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   iterator find(const_reference value)
   {  return this->find(value, this->comp()); }

   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return iterator
         (node_algorithms::find(this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   const_iterator find(const_reference value) const
   {  return this->find(value, this->comp()); }

   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      return const_iterator
         (node_algorithms::find(this->header_ptr(), key, key_node_comp), this->real_value_traits_ptr());
   }

   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return this->equal_range(value, this->comp());   }

   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::equal_range(this->header_ptr(), key, key_node_comp));
      return std::pair<iterator, iterator>( iterator(ret.first, this->real_value_traits_ptr())
                                          , iterator(ret.second, this->real_value_traits_ptr()));
   }

   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return this->equal_range(value, this->comp());   }

   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::equal_range(this->header_ptr(), key, key_node_comp));
      return std::pair<const_iterator, const_iterator>( const_iterator(ret.first, this->real_value_traits_ptr())
                                                      , const_iterator(ret.second, this->real_value_traits_ptr()));
   }

   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed)
   {  return this->bounded_range(lower_value, upper_value, this->comp(), left_closed, right_closed);   }

   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyValueCompare comp, bool left_closed, bool right_closed)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::bounded_range
            (this->header_ptr(), lower_key, upper_key, key_node_comp, left_closed, right_closed));
      return std::pair<iterator, iterator>( iterator(ret.first, this->real_value_traits_ptr())
                                          , iterator(ret.second, this->real_value_traits_ptr()));
   }

   std::pair<const_iterator,const_iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const
   {  return this->bounded_range(lower_value, upper_value, this->comp(), left_closed, right_closed);   }

   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator,const_iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         key_node_comp(comp, &this->get_real_value_traits());
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::bounded_range
            (this->header_ptr(), lower_key, upper_key, key_node_comp, left_closed, right_closed));
      return std::pair<const_iterator, const_iterator>( const_iterator(ret.first, this->real_value_traits_ptr())
                                                      , const_iterator(ret.second, this->real_value_traits_ptr()));
   }

   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         ocomp(key_value_comp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->header_ptr(), key, ocomp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }

   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         ocomp(key_value_comp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->header_ptr(), hint.pointed_node(), key, ocomp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }
};

template<class ValueTraits, class VoidOrKeyComp, bool ConstantTimeSize, class SizeType, algo_types AlgoType>
struct bstbase
   : public detail::size_holder<ConstantTimeSize, SizeType>
   , public bstbase2 < ValueTraits, VoidOrKeyComp, AlgoType>
{
   typedef typename detail::get_real_value_traits<ValueTraits>::type real_value_traits;
   typedef bstbase2< ValueTraits, VoidOrKeyComp, AlgoType> base_type;
   typedef typename base_type::value_compare       value_compare;
   typedef BOOST_INTRUSIVE_IMPDEF(value_compare)   key_compare;
   typedef typename base_type::const_reference     const_reference;
   typedef typename base_type::reference           reference;
   typedef typename base_type::iterator            iterator;
   typedef typename base_type::const_iterator      const_iterator;
   typedef typename base_type::node_traits         node_traits;
   typedef typename get_algo
      <AlgoType, node_traits>::type                algo_type;
   typedef SizeType                                size_type;

   bstbase(const value_compare & comp, const ValueTraits &vtraits)
      : base_type(comp, vtraits)
   {}

   public:
   typedef detail::size_holder<ConstantTimeSize, SizeType>     size_traits;

   size_traits &sz_traits()
   {  return *this;  }

   const size_traits &sz_traits() const
   {  return *this;  }

   size_type count(const_reference value) const
   {  return size_type(this->count(value, this->comp()));   }

   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType &key, KeyValueCompare comp) const
   {
      std::pair<const_iterator, const_iterator> ret = this->equal_range(key, comp);
      return size_type(std::distance(ret.first, ret.second));
   }

   bool empty() const
   {
      if(ConstantTimeSize){
         return !this->sz_traits().get_size();
      }
      else{
         return algo_type::unique(this->header_ptr());
      }
   }
};


/// @endcond

//! The class template bstree is an unbalanced intrusive binary search tree
//! container. The no-throw guarantee holds only, if the value_compare object
//! doesn't throw.
//!
//! The complexity guarantees only hold if the tree is balanced, logarithmic
//! complexity would increase to linear if the tree is totally unbalanced.
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
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
class bstree_impl
   :  public bstbase<ValueTraits, VoidKeyComp, ConstantTimeSize, SizeType, AlgoType>
   ,  private detail::clear_on_destructor_base
         < bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> 
         , is_safe_autounlink<detail::get_real_value_traits<ValueTraits>::type::link_mode>::value
         >
{
   template<class C, bool> friend class detail::clear_on_destructor_base;
   public:
   typedef ValueTraits value_traits;
   /// @cond
   static const bool external_value_traits =
      detail::external_value_traits_bool_is_true<value_traits>::value;
   typedef typename detail::get_real_value_traits<ValueTraits>::type real_value_traits;
   typedef bstbase<value_traits, VoidKeyComp, ConstantTimeSize, SizeType, AlgoType> data_type;
   typedef tree_iterator<real_value_traits, false> iterator_type;
   typedef tree_iterator<real_value_traits, true>  const_iterator_type;
   /// @endcond

   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::pointer)                          pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::const_pointer)                    const_pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::element_type)               value_type;
   typedef BOOST_INTRUSIVE_IMPDEF(value_type)                                                   key_type;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<pointer>::reference)                  reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::reference)            const_reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename pointer_traits<const_pointer>::difference_type)      difference_type;
   typedef BOOST_INTRUSIVE_IMPDEF(SizeType)                                                     size_type;
   typedef BOOST_INTRUSIVE_IMPDEF(typename data_type::value_compare)                            value_compare;
   typedef BOOST_INTRUSIVE_IMPDEF(value_compare)                                                key_compare;
   typedef BOOST_INTRUSIVE_IMPDEF(iterator_type)                                                iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(const_iterator_type)                                          const_iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(boost::intrusive::detail::reverse_iterator<iterator>)         reverse_iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(boost::intrusive::detail::reverse_iterator<const_iterator>)   const_reverse_iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(typename real_value_traits::node_traits)                      node_traits;
   typedef BOOST_INTRUSIVE_IMPDEF(typename node_traits::node)                                   node;
   typedef BOOST_INTRUSIVE_IMPDEF(typename node_traits::node_ptr)                               node_ptr;
   typedef BOOST_INTRUSIVE_IMPDEF(typename node_traits::const_node_ptr)                         const_node_ptr;
   /// @cond
   typedef typename get_algo<AlgoType, node_traits>::type                                       algo_type;
   /// @endcond
   typedef BOOST_INTRUSIVE_IMPDEF(algo_type)                                                    node_algorithms;

   static const bool constant_time_size = ConstantTimeSize;
   static const bool stateful_value_traits = detail::is_stateful_value_traits<real_value_traits>::value;
   /// @cond
   private:

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(bstree_impl)

   static const bool safemode_or_autounlink = is_safe_autounlink<real_value_traits::link_mode>::value;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_STATIC_ASSERT(!(constant_time_size && ((int)real_value_traits::link_mode == (int)auto_unlink)));


   protected:


   /// @endcond

   public:

   typedef typename node_algorithms::insert_commit_data insert_commit_data;

   //! <b>Effects</b>: Constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructorof the value_compare object throws. Basic guarantee.
   explicit bstree_impl( const value_compare &cmp = value_compare()
                       , const value_traits &v_traits = value_traits())
      :  data_type(cmp, v_traits)
   {
      node_algorithms::init_header(this->header_ptr());
      this->sz_traits().set_size(size_type(0));
   }

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
   //!   or the copy constructor/operator() of the value_compare object throws. Basic guarantee.
   template<class Iterator>
   bstree_impl( bool unique, Iterator b, Iterator e
              , const value_compare &cmp     = value_compare()
              , const value_traits &v_traits = value_traits())
      : data_type(cmp, v_traits)
   {
      node_algorithms::init_header(this->header_ptr());
      this->sz_traits().set_size(size_type(0));
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! <b>Effects</b>: to-do
   //!
   bstree_impl(BOOST_RV_REF(bstree_impl) x)
      : data_type(::boost::move(x.comp()), ::boost::move(x.val_traits()))
   {
      node_algorithms::init_header(this->header_ptr());
      this->sz_traits().set_size(size_type(0));
      this->swap(x);
   }

   //! <b>Effects</b>: to-do
   //!
   bstree_impl& operator=(BOOST_RV_REF(bstree_impl) x)
   {  this->swap(x); return *this;  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! <b>Effects</b>: Detaches all elements from this. The objects in the set
   //!   are not deleted (i.e. no destructors are called), but the nodes according to
   //!   the value_traits template parameter are reinitialized and thus can be reused.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this.
   //!
   //! <b>Throws</b>: Nothing.
   ~bstree_impl()
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator begin();

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const;

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const;

   //! <b>Effects</b>: Returns an iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator end();

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator end() const;

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin();

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend();

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   static bstree_impl &container_from_end_iterator(iterator end_iterator)
   {
      return *static_cast<bstree_impl*>
               (boost::intrusive::detail::to_raw_pointer(end_iterator.pointed_node()));
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   static const bstree_impl &container_from_end_iterator(const_iterator end_iterator)
   {
      return *static_cast<const bstree_impl*>
               (boost::intrusive::detail::to_raw_pointer(end_iterator.pointed_node()));
   }

   //! <b>Precondition</b>: it must be a valid iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic.
   static bstree_impl &container_from_iterator(iterator it)
   {  return container_from_end_iterator(it.end_iterator_from_it());   }

   //! <b>Precondition</b>: it must be a valid end const_iterator
   //!   of container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic.
   static const bstree_impl &container_from_iterator(const_iterator it)
   {  return container_from_end_iterator(it.end_iterator_from_it());   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns the key_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   key_compare key_comp() const;
   
   //! <b>Effects</b>: Returns the value_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const;

   //! <b>Effects</b>: Returns true if the container is empty.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   bool empty() const;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns the number of elements stored in the container.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this
   //!   if constant-time size option is disabled. Constant time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   size_type size() const
   {
      if(constant_time_size)
         return this->sz_traits().get_size();
      else{
         return (size_type)node_algorithms::size(this->header_ptr());
      }
   }

   //! <b>Effects</b>: Swaps the contents of two containers.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the comparison functor's swap call throws.
   void swap(bstree_impl& other)
   {
      //This can throw
      using std::swap;
      swap(this->comp(), this->comp());
      //These can't throw
      node_algorithms::swap_tree(this->header_ptr(), node_ptr(other.header_ptr()));
      if(constant_time_size){
         size_type backup = this->sz_traits().get_size();
         this->sz_traits().set_size(other.sz_traits().get_size());
         other.sz_traits().set_size(backup);
      }
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
   void clone_from(const bstree_impl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!src.empty()){
         detail::exception_disposer<bstree_impl, Disposer>
            rollback(*this, disposer);
         node_algorithms::clone
            (const_node_ptr(src.header_ptr())
            ,node_ptr(this->header_ptr())
            ,detail::node_cloner <Cloner,    real_value_traits, AlgoType>(cloner,   &this->get_real_value_traits())
            ,detail::node_disposer<Disposer, real_value_traits, AlgoType>(disposer, &this->get_real_value_traits()));
         this->sz_traits().set_size(src.sz_traits().get_size());
         this->comp() = src.comp();
         rollback.release();
      }
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container before the upper bound.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->comp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      iterator ret(node_algorithms::insert_equal_upper_bound
         (this->header_ptr(), to_insert, key_node_comp), this->real_value_traits_ptr());
      this->sz_traits().increment();
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
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(const_iterator hint, reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->comp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      iterator ret(node_algorithms::insert_equal
         (this->header_ptr(), hint.pointed_node(), to_insert, key_node_comp), this->real_value_traits_ptr());
      this->sz_traits().increment();
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
   //! <b>Throws</b>: Nothing.
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
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert_unique(reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = this->insert_unique_check(value, this->comp(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<iterator, bool> (this->insert_unique_commit(value, commit_data), true);
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
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_unique(const_iterator hint, reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = this->insert_unique_check(hint, value, this->comp(), commit_data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(value, commit_data);
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
   //! <b>Throws</b>: Nothing.
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

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
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
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data);

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
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
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the container between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //! <b>Effects</b>: Inserts the value in the container using the information obtained
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
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      node_algorithms::insert_unique_commit
               (this->header_ptr(), to_insert, commit_data);
      this->sz_traits().increment();
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
   //! <b>Throws</b>: Nothing.
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
      this->sz_traits().increment();
      return iterator(node_algorithms::insert_before
         (this->header_ptr(), pos.pointed_node(), to_insert), this->real_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the last position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
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
      this->sz_traits().increment();
      node_algorithms::push_back(this->header_ptr(), to_insert);
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the first position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
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
      this->sz_traits().increment();
      node_algorithms::push_front(this->header_ptr(), to_insert);
   }

   //! <b>Effects</b>: Erases the element pointed to by pos.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: Nothing.
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
      node_algorithms::erase(this->header_ptr(), to_erase);
      this->sz_traits().decrement();
      if(safemode_or_autounlink)
         node_algorithms::init(to_erase);
      return ret.unconst();
   }

   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  size_type n;   return this->private_erase(b, e, n);   }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return this->erase(value, this->comp());   }

   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
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
      this->private_erase(p.first, p.second, n);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
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
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {
      std::pair<iterator,iterator> p = this->equal_range(value);
      size_type n;
      this->private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  size_type n;   return this->private_erase(b, e, n, disposer);   }

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
   //! <b>Throws</b>: Nothing.
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
      this->private_erase(p.first, p.second, n, disposer);
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
   {
      if(safemode_or_autounlink){
         this->clear_and_dispose(detail::null_disposer());
      }
      else{
         node_algorithms::init_header(this->header_ptr());
         this->sz_traits().set_size(0);
      }
   }

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
      node_algorithms::clear_and_dispose(this->header_ptr()
         , detail::node_disposer<Disposer, real_value_traits, AlgoType>(disposer, &this->get_real_value_traits()));
      node_algorithms::init_header(this->header_ptr());
      this->sz_traits().set_size(0);
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //!
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given value.
   //!
   //! <b>Throws</b>: Nothing.
   size_type count(const_reference value) const;

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //!
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType &key, KeyValueCompare comp) const;

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   iterator lower_bound(const_reference value);

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator lower_bound(const_reference value) const;

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType &key, KeyValueCompare comp);
   
   //! <b>Effects</b>: Returns a const iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType &key, KeyValueCompare comp) const;

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   iterator upper_bound(const_reference value);

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k according to comp or end() if that element
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType &key, KeyValueCompare comp);

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator upper_bound(const_reference value) const;

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k according to comp or end() if that element
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType &key, KeyValueCompare comp) const;

   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   iterator find(const_reference value);

   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType &key, KeyValueCompare comp);

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator find(const_reference value) const;

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType &key, KeyValueCompare comp) const;

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType &key, KeyValueCompare comp);

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType &key, KeyValueCompare comp) const;

   //! <b>Requires</b>: 'lower_value' must not be greater than 'upper_value'. If
   //!   'lower_value' == 'upper_value', ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key) if left_closed, upper_bound(lower_key) otherwise
   //!
   //!   second = upper_bound(upper_key) if right_closed, lower_bound(upper_key) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If the predicate throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_value and upper_value.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! <b>Requires</b>: KeyValueCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the container. 
   //!   'lower_key' must not be greater than 'upper_key' according to 'comp'. If
   //!   'lower_key' == 'upper_key', ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key, comp) if left_closed, upper_bound(lower_key, comp) otherwise
   //!
   //!   second = upper_bound(upper_key, comp) if right_closed, lower_bound(upper_key, comp) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_key and upper_key.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! <b>Requires</b>: 'lower_value' must not be greater than 'upper_value'. If
   //!   'lower_value' == 'upper_value', ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key) if left_closed, upper_bound(lower_key) otherwise
   //!
   //!   second = upper_bound(upper_key) if right_closed, lower_bound(upper_key) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If the predicate throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_value and upper_value.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   std::pair<const_iterator,const_iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! <b>Requires</b>: KeyValueCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the container. 
   //!   'lower_key' must not be greater than 'upper_key' according to 'comp'. If
   //!   'lower_key' == 'upper_key', ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key, comp) if left_closed, upper_bound(lower_key, comp) otherwise
   //!
   //!   second = upper_bound(upper_key, comp) if right_closed, lower_bound(upper_key, comp) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_key and upper_key.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator,const_iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

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
   static iterator s_iterator_to(reference value);

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
   static const_iterator s_iterator_to(const_reference value);

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value);

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const;

   //! <b>Requires</b>: value shall not be in a container.
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
   static void init_node(reference value);

   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Unlinks the leftmost node from the container.
   //!
   //! <b>Complexity</b>: Average complexity is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function breaks the container and the container can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the container.
   pointer unlink_leftmost_without_rebalance()
   {
      node_ptr to_be_disposed(node_algorithms::unlink_leftmost_without_rebalance
                           (this->header_ptr()));
      if(!to_be_disposed)
         return 0;
      this->sz_traits().decrement();
      if(safemode_or_autounlink)//If this is commented does not work with normal_link
         node_algorithms::init(to_be_disposed);
      return this->get_real_value_traits().to_value_ptr(to_be_disposed);
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Requires</b>: replace_this must be a valid iterator of *this
   //!   and with_this must not be inserted in any container.
   //!
   //! <b>Effects</b>: Replaces replace_this in its position in the
   //!   container with with_this. The container does not need to be rebalanced.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   with_this is not equivalent to *replace_this according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   void replace_node(iterator replace_this, reference with_this);

   //! <b>Effects</b>: Rebalances the tree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear.
   void rebalance();

   //! <b>Requires</b>: old_root is a node of a tree.
   //!
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   iterator rebalance_subtree(iterator root);

   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: removes "value" from the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic time.
   //!
   //! <b>Note</b>: This static function is only usable with non-constant
   //! time size containers that have stateless comparison functors.
   //!
   //! If the user calls
   //! this function with a constant time size container or stateful comparison
   //! functor a compilation error will be issued.
   static void remove_node(reference value)
   {
      BOOST_STATIC_ASSERT((!constant_time_size));
      node_ptr to_remove(value_traits::to_node_ptr(value));
      node_algorithms::unlink(to_remove);
      if(safemode_or_autounlink)
         node_algorithms::init(to_remove);
   }

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

   private:
   static bstree_impl &priv_container_from_end_iterator(const const_iterator &end_iterator)
   {
      return *static_cast<bstree_impl*>
         (boost::intrusive::detail::to_raw_pointer(end_iterator.pointed_node()));
   }
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline bool operator<
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
bool operator==
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{
   typedef bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> tree_type;
   typedef typename tree_type::const_iterator const_iterator;

   if(tree_type::constant_time_size && x.size() != y.size()){
      return false;
   }
   const_iterator end1 = x.end();
   const_iterator i1 = x.begin();
   const_iterator i2 = y.begin();
   if(tree_type::constant_time_size){
      while (i1 != end1 && *i1 == *i2) {
         ++i1;
         ++i2;
      }
      return i1 == end1;
   }
   else{
      const_iterator end2 = y.end();
      while (i1 != end1 && i2 != end2 && *i1 == *i2) {
         ++i1;
         ++i2;
      }
      return i1 == end1 && i2 == end2;
   }
}

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline bool operator!=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const bstree_impl<T, Options...> &x, const bstree_impl<T, Options...> &y)
#else
( const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, const bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidKeyComp, class SizeType, bool ConstantTimeSize, algo_types AlgoType>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(bstree_impl<T, Options...> &x, bstree_impl<T, Options...> &y)
#else
( bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &x
, bstree_impl<ValueTraits, VoidKeyComp, SizeType, ConstantTimeSize, AlgoType> &y)
#endif
{  x.swap(y);  }

//! Helper metafunction to define a \c bstree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void>
#endif
struct make_bstree
{
   /// @cond
   typedef typename pack_options
      < bstree_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;

   typedef bstree_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::size_type
         , packed_options::constant_time_size
         , BsTreeAlgorithms
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
class bstree
   :  public make_bstree<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_bstree
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(bstree)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::real_value_traits  real_value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename real_value_traits::value_type, T>::value));

   bstree( const value_compare &cmp = value_compare()
         , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   bstree( bool unique, Iterator b, Iterator e
         , const value_compare &cmp = value_compare()
         , const value_traits &v_traits = value_traits())
      :  Base(unique, b, e, cmp, v_traits)
   {}

   bstree(BOOST_RV_REF(bstree) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   bstree& operator=(BOOST_RV_REF(bstree) x)
   {  return static_cast<bstree &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static bstree &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<bstree &>(Base::container_from_end_iterator(end_iterator));   }

   static const bstree &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const bstree &>(Base::container_from_end_iterator(end_iterator));   }

   static bstree &container_from_iterator(iterator it)
   {  return static_cast<bstree &>(Base::container_from_iterator(it));   }

   static const bstree &container_from_iterator(const_iterator it)
   {  return static_cast<const bstree &>(Base::container_from_iterator(it));   }
};

#endif
} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_BSTREE_HPP
