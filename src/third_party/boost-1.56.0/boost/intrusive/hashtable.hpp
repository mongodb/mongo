/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_HASHTABLE_HPP
#define BOOST_INTRUSIVE_HASHTABLE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
//std C++
#include <functional>   //std::equal_to
#include <utility>      //std::pair
#include <algorithm>    //std::swap, std::lower_bound, std::upper_bound
#include <cstddef>      //std::size_t
//boost
#include <boost/intrusive/detail/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/functional/hash.hpp>
//General intrusive utilities
#include <boost/intrusive/detail/hashtable_node.hpp>
#include <boost/intrusive/detail/transform_iterator.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/detail/utilities.hpp>
//Implementation utilities
#include <boost/intrusive/unordered_set_hook.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/move/move.hpp>

namespace boost {
namespace intrusive {

/// @cond

struct hash_bool_flags
{
   static const std::size_t unique_keys_pos        = 1u;
   static const std::size_t constant_time_size_pos = 2u;
   static const std::size_t power_2_buckets_pos    = 4u;
   static const std::size_t cache_begin_pos        = 8u;
   static const std::size_t compare_hash_pos       = 16u;
   static const std::size_t incremental_pos        = 32u;
};

namespace detail {

template<class SupposedValueTraits>
struct get_slist_impl_from_supposed_value_traits
{
   typedef SupposedValueTraits                  value_traits;
   typedef typename detail::get_node_traits
      <value_traits>::type                      node_traits;
   typedef typename get_slist_impl
      <typename reduced_slist_node_traits
         <node_traits>::type
      >::type                                   type;
};

template<class SupposedValueTraits>
struct unordered_bucket_impl
{
   typedef typename
      get_slist_impl_from_supposed_value_traits
         <SupposedValueTraits>::type            slist_impl;
   typedef detail::bucket_impl<slist_impl>      implementation_defined;
   typedef implementation_defined               type;
};

template<class SupposedValueTraits>
struct unordered_bucket_ptr_impl
{
   typedef typename detail::get_node_traits
      <SupposedValueTraits>::type::node_ptr     node_ptr;
   typedef typename unordered_bucket_impl
      <SupposedValueTraits>::type               bucket_type;

   typedef typename pointer_traits
      <node_ptr>::template rebind_pointer
         < bucket_type >::type                  implementation_defined;
   typedef implementation_defined               type;
};

template <class T>
struct store_hash_bool
{
   template<bool Add>
   struct two_or_three {one _[2 + Add];};
   template <class U> static one test(...);
   template <class U> static two_or_three<U::store_hash> test (int);
   static const std::size_t value = sizeof(test<T>(0));
};

template <class T>
struct store_hash_is_true
{
   static const bool value = store_hash_bool<T>::value > sizeof(one)*2;
};

template <class T>
struct optimize_multikey_bool
{
   template<bool Add>
   struct two_or_three {one _[2 + Add];};
   template <class U> static one test(...);
   template <class U> static two_or_three<U::optimize_multikey> test (int);
   static const std::size_t value = sizeof(test<T>(0));
};

template <class T>
struct optimize_multikey_is_true
{
   static const bool value = optimize_multikey_bool<T>::value > sizeof(one)*2;
};

struct insert_commit_data_impl
{
   std::size_t hash;
};

template<class Node, class SlistNodePtr>
inline typename pointer_traits<SlistNodePtr>::template rebind_pointer<Node>::type
   dcast_bucket_ptr(const SlistNodePtr &p)
{
   typedef typename pointer_traits<SlistNodePtr>::template rebind_pointer<Node>::type node_ptr;
   return pointer_traits<node_ptr>::pointer_to(static_cast<Node&>(*p));
}

template<class NodeTraits>
struct group_functions
{
   typedef NodeTraits                                             node_traits;
   typedef unordered_group_adapter<node_traits>                   group_traits;
   typedef typename node_traits::node_ptr                         node_ptr;
   typedef typename node_traits::node                             node;
   typedef typename reduced_slist_node_traits
      <node_traits>::type                                         reduced_node_traits;
   typedef typename reduced_node_traits::node_ptr                 slist_node_ptr;
   typedef typename reduced_node_traits::node                     slist_node;
   typedef circular_slist_algorithms<group_traits>                group_algorithms;

   static slist_node_ptr get_bucket_before_begin
      (const slist_node_ptr &bucket_beg, const slist_node_ptr &bucket_end, const node_ptr &p)
   {
      //First find the last node of p's group.
      //This requires checking the first node of the next group or
      //the bucket node.
      node_ptr prev_node = p;
      node_ptr nxt(node_traits::get_next(p));
      while(!(bucket_beg <= nxt && nxt <= bucket_end) &&
             (group_traits::get_next(nxt) == prev_node)){
         prev_node = nxt;
         nxt = node_traits::get_next(nxt);
      }

      //If we've reached the bucket node just return it.
      if(bucket_beg <= nxt && nxt <= bucket_end){
         return nxt;
      }

      //Otherwise, iterate using group links until the bucket node
      node_ptr first_node_of_group  = nxt;
      node_ptr last_node_group      = group_traits::get_next(first_node_of_group);
      slist_node_ptr possible_end   = node_traits::get_next(last_node_group);

      while(!(bucket_beg <= possible_end && possible_end <= bucket_end)){
         first_node_of_group = detail::dcast_bucket_ptr<node>(possible_end);
         last_node_group   = group_traits::get_next(first_node_of_group);
         possible_end      = node_traits::get_next(last_node_group);
      }
      return possible_end;
   }

   static node_ptr get_prev_to_first_in_group(const slist_node_ptr &bucket_node, const node_ptr &first_in_group)
   {
      //Just iterate using group links and obtain the node
      //before "first_in_group)"
      node_ptr prev_node = detail::dcast_bucket_ptr<node>(bucket_node);
      node_ptr nxt(node_traits::get_next(prev_node));
      while(nxt != first_in_group){
         prev_node = group_traits::get_next(nxt);
         nxt = node_traits::get_next(prev_node);
      }
      return prev_node;
   }

   static node_ptr get_first_in_group_of_last_in_group(const node_ptr &last_in_group)
   {
      //Just iterate using group links and obtain the node
      //before "last_in_group"
      node_ptr possible_first = group_traits::get_next(last_in_group);
      node_ptr possible_first_prev = group_traits::get_next(possible_first);
      // The deleted node is at the end of the group, so the
      // node in the group pointing to it is at the beginning
      // of the group. Find that to change its pointer.
      while(possible_first_prev != last_in_group){
         possible_first = possible_first_prev;
         possible_first_prev = group_traits::get_next(possible_first);
      }
      return possible_first;
   }

   static void erase_from_group(const slist_node_ptr &end_ptr, const node_ptr &to_erase_ptr, detail::true_)
   {
      node_ptr nxt_ptr(node_traits::get_next(to_erase_ptr));
      node_ptr prev_in_group_ptr(group_traits::get_next(to_erase_ptr));
      bool last_in_group = (end_ptr == nxt_ptr) ||
         (group_traits::get_next(nxt_ptr) != to_erase_ptr);
      bool is_first_in_group = node_traits::get_next(prev_in_group_ptr) != to_erase_ptr;

      if(is_first_in_group && last_in_group){
         group_algorithms::init(to_erase_ptr);
      }
      else if(is_first_in_group){
         group_algorithms::unlink_after(nxt_ptr);
      }
      else if(last_in_group){
         node_ptr first_in_group =
            get_first_in_group_of_last_in_group(to_erase_ptr);
         group_algorithms::unlink_after(first_in_group);
      }
      else{
         group_algorithms::unlink_after(nxt_ptr);
      }
   }

   static void erase_from_group(const slist_node_ptr&, const node_ptr&, detail::false_)
   {}

   static node_ptr get_last_in_group(const node_ptr &first_in_group, detail::true_)
   {  return group_traits::get_next(first_in_group);  }

   static node_ptr get_last_in_group(const node_ptr &n, detail::false_)
   {  return n;  }

   static void init_group(const node_ptr &n, true_)
   {  group_algorithms::init(n); }

   static void init_group(const node_ptr &, false_)
   {}

   static void insert_in_group(const node_ptr &first_in_group, const node_ptr &n, true_)
   {
      if(first_in_group){
         if(group_algorithms::unique(first_in_group))
            group_algorithms::link_after(first_in_group, n);
         else{
            group_algorithms::link_after(group_algorithms::node_traits::get_next(first_in_group), n);
         }
      }
      else{
         group_algorithms::init_header(n);
      }
   }

   static slist_node_ptr get_previous_and_next_in_group
      ( const slist_node_ptr &i, node_ptr &nxt_in_group
      //If first_end_ptr == last_end_ptr, then first_end_ptr is the bucket of i
      //Otherwise first_end_ptr is the first bucket and last_end_ptr the last one.
      , const slist_node_ptr &first_end_ptr, const slist_node_ptr &last_end_ptr)
   {
      slist_node_ptr prev;
      node_ptr elem(detail::dcast_bucket_ptr<node>(i));

      //It's the last in group if the next_node is a bucket
      slist_node_ptr nxt(node_traits::get_next(elem));
      bool last_in_group = (first_end_ptr <= nxt && nxt <= last_end_ptr)/* ||
                            (group_traits::get_next(nxt) != elem)*/;
      //It's the first in group if group_previous's next_node is not
      //itself, as group list does not link bucket
      node_ptr prev_in_group(group_traits::get_next(elem));
      bool first_in_group = node_traits::get_next(prev_in_group) != elem;

      if(first_in_group){
         node_ptr start_pos;
         if(last_in_group){
            start_pos = elem;
            nxt_in_group = node_ptr();
         }
         else{
            start_pos = prev_in_group;
            nxt_in_group = node_traits::get_next(elem);
         }
         slist_node_ptr bucket_node;
         if(first_end_ptr != last_end_ptr){
            bucket_node = group_functions::get_bucket_before_begin
                  (first_end_ptr, last_end_ptr, start_pos);
         }
         else{
            bucket_node = first_end_ptr;
         }
         prev = group_functions::get_prev_to_first_in_group(bucket_node, elem);
      }
      else{
         if(last_in_group){
            nxt_in_group = group_functions::get_first_in_group_of_last_in_group(elem);
         }
         else{
            nxt_in_group = node_traits::get_next(elem);
         }
         prev = group_traits::get_next(elem);
      }
      return prev;
   }

   static void insert_in_group(const node_ptr&, const node_ptr&, false_)
   {}
};

template<class BucketType, class SplitTraits>
class incremental_rehash_rollback
{
   private:
   typedef BucketType   bucket_type;
   typedef SplitTraits  split_traits;

   incremental_rehash_rollback();
   incremental_rehash_rollback & operator=(const incremental_rehash_rollback &);
   incremental_rehash_rollback (const incremental_rehash_rollback &);

   public:
   incremental_rehash_rollback
      (bucket_type &source_bucket, bucket_type &destiny_bucket, split_traits &split_traits)
      :  source_bucket_(source_bucket),  destiny_bucket_(destiny_bucket)
      ,  split_traits_(split_traits),  released_(false)
   {}

   void release()
   {  released_ = true; }

   ~incremental_rehash_rollback()
   {
      if(!released_){
         //If an exception is thrown, just put all moved nodes back in the old bucket
         //and move back the split mark.
         destiny_bucket_.splice_after(destiny_bucket_.before_begin(), source_bucket_);
         split_traits_.decrement();
      }
   }

   private:
   bucket_type &source_bucket_;
   bucket_type &destiny_bucket_;
   split_traits &split_traits_;
   bool released_;
};

template<class NodeTraits>
struct node_functions
{
   static void store_hash(typename NodeTraits::node_ptr p, std::size_t h, true_)
   {  return NodeTraits::set_hash(p, h); }

   static void store_hash(typename NodeTraits::node_ptr, std::size_t, false_)
   {}
};

inline std::size_t hash_to_bucket(std::size_t hash_value, std::size_t bucket_cnt, detail::false_)
{  return hash_value % bucket_cnt;  }

inline std::size_t hash_to_bucket(std::size_t hash_value, std::size_t bucket_cnt, detail::true_)
{  return hash_value & (bucket_cnt - 1);   }

template<bool Power2Buckets, bool Incremental>
inline std::size_t hash_to_bucket_split(std::size_t hash_value, std::size_t bucket_cnt, std::size_t split)
{
   std::size_t bucket_number = detail::hash_to_bucket(hash_value, bucket_cnt, detail::bool_<Power2Buckets>());
   if(Incremental)
      if(bucket_number >= split)
         bucket_number -= bucket_cnt/2;
   return bucket_number;
}

}  //namespace detail {

//!This metafunction will obtain the type of a bucket
//!from the value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct unordered_bucket
   : public detail::unordered_bucket_impl
      <typename ValueTraitsOrHookOption::
         template pack<none>::proto_value_traits
      >
{};

//!This metafunction will obtain the type of a bucket pointer
//!from the value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct unordered_bucket_ptr
   :  public detail::unordered_bucket_ptr_impl
         <typename ValueTraitsOrHookOption::
          template pack<none>::proto_value_traits
         >
{};

//!This metafunction will obtain the type of the default bucket traits
//!(when the user does not specify the bucket_traits<> option) from the
//!value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct unordered_default_bucket_traits
{
   typedef typename ValueTraitsOrHookOption::
      template pack<none>::proto_value_traits   supposed_value_traits;
   typedef typename detail::
      get_slist_impl_from_supposed_value_traits
         <supposed_value_traits>::type          slist_impl;
   typedef detail::bucket_traits_impl
      <slist_impl>                              implementation_defined;
   typedef implementation_defined               type;
};

struct default_bucket_traits;

struct hashtable_defaults
{
   typedef detail::default_hashtable_hook   proto_value_traits;
   typedef std::size_t                 size_type;
   typedef void                        equal;
   typedef void                        hash;
   typedef default_bucket_traits       bucket_traits;
   static const bool constant_time_size   = true;
   static const bool power_2_buckets      = false;
   static const bool cache_begin          = false;
   static const bool compare_hash         = false;
   static const bool incremental          = false;
};

template<class ValueTraits, bool IsConst>
struct downcast_node_to_value_t
   :  public detail::node_to_value<ValueTraits, IsConst>
{
   typedef detail::node_to_value<ValueTraits, IsConst>  base_t;
   typedef typename base_t::result_type                     result_type;
   typedef ValueTraits                                  value_traits;
   typedef typename detail::get_slist_impl
      <typename detail::reduced_slist_node_traits
         <typename value_traits::node_traits>::type
      >::type                                               slist_impl;
   typedef typename detail::add_const_if_c
         <typename slist_impl::node, IsConst>::type      &  first_argument_type;
   typedef typename detail::add_const_if_c
         < typename ValueTraits::node_traits::node
         , IsConst>::type                                &  intermediate_argument_type;
   typedef typename pointer_traits
      <typename ValueTraits::pointer>::
         template rebind_pointer
            <const ValueTraits>::type                   const_value_traits_ptr;

   downcast_node_to_value_t(const const_value_traits_ptr &ptr)
      :  base_t(ptr)
   {}

   result_type operator()(first_argument_type arg) const
   {  return this->base_t::operator()(static_cast<intermediate_argument_type>(arg)); }
};

template<class F, class SlistNodePtr, class NodePtr>
struct node_cast_adaptor
   :  private detail::ebo_functor_holder<F>
{
   typedef detail::ebo_functor_holder<F> base_t;

   typedef typename pointer_traits<SlistNodePtr>::element_type slist_node;
   typedef typename pointer_traits<NodePtr>::element_type      node;

   template<class ConvertibleToF, class RealValuTraits>
   node_cast_adaptor(const ConvertibleToF &c2f, const RealValuTraits *traits)
      :  base_t(base_t(c2f, traits))
   {}

   typename base_t::node_ptr operator()(const slist_node &to_clone)
   {  return base_t::operator()(static_cast<const node &>(to_clone));   }

   void operator()(SlistNodePtr to_clone)
   {
      base_t::operator()(pointer_traits<NodePtr>::pointer_to(static_cast<node &>(*to_clone)));
   }
};

static const std::size_t hashtable_data_bool_flags_mask  =
   ( hash_bool_flags::cache_begin_pos
   | hash_bool_flags::constant_time_size_pos
   | hash_bool_flags::incremental_pos
   );

//bucket_plus_vtraits stores ValueTraits + BucketTraits
//this data is needed by iterators to obtain the
//value from the iterator and detect the bucket
template<class ValueTraits, class BucketTraits>
struct bucket_plus_vtraits : public ValueTraits
{
   typedef BucketTraits bucket_traits;
   typedef ValueTraits  value_traits;

   static const bool safemode_or_autounlink = is_safe_autounlink<value_traits::link_mode>::value;

   typedef typename
      detail::get_slist_impl_from_supposed_value_traits
         <value_traits>::type                            slist_impl;
   typedef typename value_traits::node_traits            node_traits;
   typedef unordered_group_adapter<node_traits>          group_traits;
   typedef typename slist_impl::iterator                 siterator;
   typedef typename slist_impl::size_type                size_type;
   typedef detail::bucket_impl<slist_impl>               bucket_type;
   typedef detail::group_functions<node_traits>          group_functions_t;
   typedef typename slist_impl::node_algorithms          node_algorithms;
   typedef typename slist_impl::node_ptr                 slist_node_ptr;
   typedef typename node_traits::node_ptr                node_ptr;
   typedef typename node_traits::node                    node;
   typedef typename value_traits::value_type             value_type;
   typedef circular_slist_algorithms<group_traits>       group_algorithms;
   typedef typename pointer_traits
      <typename value_traits::pointer>::
         template rebind_pointer
            <const value_traits>::type                   const_value_traits_ptr;
   typedef typename pointer_traits
      <typename value_traits::pointer>::
         template rebind_pointer
            <const bucket_plus_vtraits>::type            const_bucket_value_traits_ptr;
   typedef typename detail::unordered_bucket_ptr_impl
      <value_traits>::type                               bucket_ptr;
   typedef detail::bool_<detail::optimize_multikey_is_true
      <node_traits>::value>                              optimize_multikey_t;

   template<class BucketTraitsType>
   bucket_plus_vtraits(const ValueTraits &val_traits, BOOST_FWD_REF(BucketTraitsType) b_traits)
      :  ValueTraits(val_traits), bucket_traits_(::boost::forward<BucketTraitsType>(b_traits))
   {}

   bucket_plus_vtraits & operator =(const bucket_plus_vtraits &x)
   {  bucket_traits_ = x.bucket_traits_;  return *this;  }

   const_value_traits_ptr priv_value_traits_ptr() const
   {  return pointer_traits<const_value_traits_ptr>::pointer_to(this->priv_value_traits());  }

   //bucket_value_traits
   //
   const bucket_plus_vtraits &get_bucket_value_traits() const
   {  return *this;  }

   bucket_plus_vtraits &get_bucket_value_traits()
   {  return *this;  }

   const_bucket_value_traits_ptr bucket_value_traits_ptr() const
   {  return pointer_traits<const_bucket_value_traits_ptr>::pointer_to(this->get_bucket_value_traits());  }

   //value traits
   //
   const value_traits &priv_value_traits() const
   {  return *this;  }

   value_traits &priv_value_traits()
   {  return *this;  }

   //bucket_traits
   //
   const bucket_traits &priv_bucket_traits() const
   {  return this->bucket_traits_;  }

   bucket_traits &priv_bucket_traits()
   {  return this->bucket_traits_;  }

   //bucket operations
   bucket_ptr priv_bucket_pointer() const
   {  return this->priv_bucket_traits().bucket_begin();  }

   typename slist_impl::size_type priv_bucket_count() const
   {  return this->priv_bucket_traits().bucket_count();  }

   bucket_ptr priv_invalid_bucket() const
   {
      const bucket_traits &rbt = this->priv_bucket_traits();
      return rbt.bucket_begin() + rbt.bucket_count();
   }
   siterator priv_invalid_local_it() const
   {  return this->priv_bucket_traits().bucket_begin()->before_begin();  }

   static siterator priv_get_last(bucket_type &b, detail::true_)  //optimize multikey
   {
      //First find the last node of p's group.
      //This requires checking the first node of the next group or
      //the bucket node.
      slist_node_ptr end_ptr(b.end().pointed_node());
      node_ptr possible_end(node_traits::get_next( detail::dcast_bucket_ptr<node>(end_ptr)));
      node_ptr last_node_group(possible_end);

      while(end_ptr != possible_end){
         last_node_group   = group_traits::get_next(detail::dcast_bucket_ptr<node>(possible_end));
         possible_end      = node_traits::get_next(last_node_group);
      }
      return bucket_type::s_iterator_to(*last_node_group);
   }

   static siterator priv_get_last(bucket_type &b, detail::false_) //NOT optimize multikey
   {  return b.previous(b.end());   }

   static siterator priv_get_previous(bucket_type &b, siterator i, detail::true_)   //optimize multikey
   {
      node_ptr elem(detail::dcast_bucket_ptr<node>(i.pointed_node()));
      node_ptr prev_in_group(group_traits::get_next(elem));
      bool first_in_group = node_traits::get_next(prev_in_group) != elem;
      typename bucket_type::node &n = first_in_group
         ? *group_functions_t::get_prev_to_first_in_group(b.end().pointed_node(), elem)
         : *group_traits::get_next(elem)
         ;
      return bucket_type::s_iterator_to(n);
   }

   static siterator priv_get_previous(bucket_type &b, siterator i, detail::false_)   //NOT optimize multikey
   {  return b.previous(i);   }

   static void priv_clear_group_nodes(bucket_type &b, detail::true_) //optimize multikey
   {
      siterator it(b.begin()), itend(b.end());
      while(it != itend){
         node_ptr to_erase(detail::dcast_bucket_ptr<node>(it.pointed_node()));
         ++it;
         group_algorithms::init(to_erase);
      }
   }

   static void priv_clear_group_nodes(bucket_type &, detail::false_) //NOT optimize multikey
   {}

   std::size_t priv_get_bucket_num_no_hash_store(siterator it, detail::true_)    //optimize multikey
   {
      const bucket_ptr f(this->priv_bucket_pointer()), l(f + this->priv_bucket_count() - 1);
      slist_node_ptr bb = group_functions_t::get_bucket_before_begin
         ( f->end().pointed_node()
         , l->end().pointed_node()
         , detail::dcast_bucket_ptr<node>(it.pointed_node()));
      //Now get the bucket_impl from the iterator
      const bucket_type &b = static_cast<const bucket_type&>
         (bucket_type::slist_type::container_from_end_iterator(bucket_type::s_iterator_to(*bb)));
      //Now just calculate the index b has in the bucket array
      return static_cast<size_type>(&b - &*f);
   }

   std::size_t priv_get_bucket_num_no_hash_store(siterator it, detail::false_)   //NO optimize multikey
   {
      bucket_ptr f(this->priv_bucket_pointer()), l(f + this->priv_bucket_count() - 1);
      slist_node_ptr first_ptr(f->cend().pointed_node())
                   , last_ptr(l->cend().pointed_node());

      //The end node is embedded in the singly linked list:
      //iterate until we reach it.
      while(!(first_ptr <= it.pointed_node() && it.pointed_node() <= last_ptr)){
         ++it;
      }
      //Now get the bucket_impl from the iterator
      const bucket_type &b = static_cast<const bucket_type&>
         (bucket_type::container_from_end_iterator(it));

      //Now just calculate the index b has in the bucket array
      return static_cast<std::size_t>(&b - &*f);
   }

   static std::size_t priv_stored_hash(slist_node_ptr n, detail::true_) //store_hash
   {  return node_traits::get_hash(detail::dcast_bucket_ptr<node>(n));  }

   static std::size_t priv_stored_hash(slist_node_ptr, detail::false_)  //NO store_hash (This should never be called)
   {  BOOST_INTRUSIVE_INVARIANT_ASSERT(0);   return 0;   }

   node &priv_value_to_node(value_type &v)
   {  return *this->priv_value_traits().to_node_ptr(v);  }

   const node &priv_value_to_node(const value_type &v) const
   {  return *this->priv_value_traits().to_node_ptr(v);  }

   value_type &priv_value_from_slist_node(slist_node_ptr n)
   {  return *this->priv_value_traits().to_value_ptr(detail::dcast_bucket_ptr<node>(n)); }

   const value_type &priv_value_from_slist_node(slist_node_ptr n) const
   {  return *this->priv_value_traits().to_value_ptr(detail::dcast_bucket_ptr<node>(n)); }

   void priv_clear_buckets(const bucket_ptr buckets_ptr, const size_type bucket_cnt)
   {
      bucket_ptr buckets_it = buckets_ptr;
      for(size_type bucket_i = 0; bucket_i != bucket_cnt; ++buckets_it, ++bucket_i){
         if(safemode_or_autounlink){
            bucket_plus_vtraits::priv_clear_group_nodes(*buckets_it, optimize_multikey_t());
            buckets_it->clear_and_dispose(detail::init_disposer<node_algorithms>());
         }
         else{
            buckets_it->clear();
         }
      }
   }

   bucket_traits bucket_traits_;
};

//bucket_hash_t
//Stores bucket_plus_vtraits plust the hash function
template<class VoidOrKeyHash, class ValueTraits, class BucketTraits>
struct bucket_hash_t
   : public detail::ebo_functor_holder
   <typename get_hash< VoidOrKeyHash
                      , typename bucket_plus_vtraits<ValueTraits,BucketTraits>::value_traits::value_type
                      >::type
   >
{
   typedef typename bucket_plus_vtraits<ValueTraits,BucketTraits>::value_traits     value_traits;
   typedef typename value_traits::value_type                                        value_type;
   typedef typename value_traits::node_traits                                       node_traits;
   typedef typename get_hash< VoidOrKeyHash, value_type>::type                      hasher;
   typedef BucketTraits bucket_traits;
   typedef bucket_plus_vtraits<ValueTraits, BucketTraits> bucket_plus_vtraits_t;

   template<class BucketTraitsType>
   bucket_hash_t(const ValueTraits &val_traits, BOOST_FWD_REF(BucketTraitsType) b_traits, const hasher & h)
      :  detail::ebo_functor_holder<hasher>(h), internal(val_traits, ::boost::forward<BucketTraitsType>(b_traits))
   {}

   const hasher &priv_hasher() const
   {  return this->detail::ebo_functor_holder<hasher>::get();  }

   hasher &priv_hasher()
   {  return this->detail::ebo_functor_holder<hasher>::get();  }

   std::size_t priv_stored_or_compute_hash(const value_type &v, detail::true_) const   //For store_hash == true
   {  return node_traits::get_hash(this->internal.priv_value_traits().to_node_ptr(v));  }

   std::size_t priv_stored_or_compute_hash(const value_type &v, detail::false_) const  //For store_hash == false
   {  return this->priv_hasher()(v);   }

   bucket_plus_vtraits_t internal;  //4
};

//bucket_hash_equal_t
//Stores bucket_hash_t and the equality function when the first
//non-empty bucket shall not be cached.
template<class VoidOrKeyHash, class VoidOrKeyEqual, class ValueTraits, class BucketTraits, bool>
struct bucket_hash_equal_t
   : public detail::ebo_functor_holder //equal
   <typename get_equal_to< VoidOrKeyEqual
                         , typename bucket_plus_vtraits<ValueTraits,BucketTraits>::value_traits::value_type
                         >::type
   >
{
   typedef bucket_hash_t<VoidOrKeyHash, ValueTraits, BucketTraits>   bucket_hash_type;
   typedef bucket_plus_vtraits<ValueTraits,BucketTraits>             bucket_plus_vtraits_t;
   typedef typename bucket_plus_vtraits_t::value_traits              value_traits;
   typedef typename get_equal_to< VoidOrKeyEqual
                                , typename value_traits::value_type
                                >::type         value_equal;
   typedef typename bucket_hash_type::hasher    hasher;
   typedef BucketTraits                         bucket_traits;
   typedef typename bucket_plus_vtraits_t::slist_impl       slist_impl;
   typedef typename slist_impl::size_type                   size_type;
   typedef typename slist_impl::iterator                    siterator;
   typedef detail::bucket_impl<slist_impl>                  bucket_type;
   typedef typename detail::unordered_bucket_ptr_impl<value_traits>::type bucket_ptr;

   template<class BucketTraitsType>
   bucket_hash_equal_t(const ValueTraits &val_traits, BOOST_FWD_REF(BucketTraitsType) b_traits, const hasher & h, const value_equal &e)
      : detail::ebo_functor_holder<value_equal>(e)
      , internal(val_traits, ::boost::forward<BucketTraitsType>(b_traits), h)
   {}

   bucket_ptr priv_get_cache()
   {  return this->internal.internal.priv_bucket_pointer();   }

   void priv_set_cache(const bucket_ptr &)
   {}

   size_type priv_get_cache_bucket_num()
   {  return 0u;  }

   void priv_initialize_cache()
   {}

   void priv_swap_cache(bucket_hash_equal_t &)
   {}

   siterator priv_begin() const
   {
      size_type n = 0;
      size_type bucket_cnt = this->internal.internal.priv_bucket_count();
      for (n = 0; n < bucket_cnt; ++n){
         bucket_type &b = this->internal.internal.priv_bucket_pointer()[n];
         if(!b.empty()){
            return b.begin();
         }
      }
      return this->internal.internal.priv_invalid_local_it();
   }

   void priv_insertion_update_cache(size_type)
   {}

   void priv_erasure_update_cache_range(size_type, size_type)
   {}

   void priv_erasure_update_cache()
   {}

   const value_equal &priv_equal() const
   {  return this->detail::ebo_functor_holder<value_equal>::get();  }

   value_equal &priv_equal()
   {  return this->detail::ebo_functor_holder<value_equal>::get();  }

   bucket_hash_t<VoidOrKeyHash, ValueTraits, BucketTraits> internal; //3
};

//bucket_hash_equal_t
//Stores bucket_hash_t and the equality function when the first
//non-empty bucket shall be cached.
template<class VoidOrKeyHash, class VoidOrKeyEqual, class ValueTraits, class BucketTraits>  //cache_begin == true version
struct bucket_hash_equal_t<VoidOrKeyHash, VoidOrKeyEqual, ValueTraits, BucketTraits, true>
   : public detail::ebo_functor_holder //equal
               <typename get_equal_to< VoidOrKeyEqual
                         , typename bucket_plus_vtraits<ValueTraits,BucketTraits>::value_traits::value_type
                         >::type
               >
{
   typedef bucket_plus_vtraits<ValueTraits, BucketTraits>            bucket_plus_vtraits_t;
   typedef bucket_hash_t<VoidOrKeyHash, ValueTraits, BucketTraits>   bucket_hash_type;
   typedef typename bucket_plus_vtraits
         <ValueTraits,BucketTraits>::value_traits                    value_traits;
   typedef typename get_equal_to
      < VoidOrKeyEqual
      , typename value_traits::value_type>::type                     value_equal;
   typedef typename bucket_hash_type::hasher                         hasher;
   typedef BucketTraits                                              bucket_traits;
   typedef typename bucket_plus_vtraits_t::slist_impl::size_type     size_type;
   typedef typename bucket_plus_vtraits_t::slist_impl::iterator      siterator;

   template<class BucketTraitsType>
   bucket_hash_equal_t(const ValueTraits &val_traits, BOOST_FWD_REF(BucketTraitsType) b_traits, const hasher & h, const value_equal &e)
      : detail::ebo_functor_holder<value_equal>(e)
      , internal(val_traits, ::boost::forward<BucketTraitsType>(b_traits), h)
   {}

   typedef typename detail::unordered_bucket_ptr_impl
      <typename bucket_hash_type::value_traits>::type bucket_ptr;

   bucket_ptr &priv_get_cache()
   {  return cached_begin_;   }

   const bucket_ptr &priv_get_cache() const
   {  return cached_begin_;   }

   void priv_set_cache(const bucket_ptr &p)
   {  cached_begin_ = p;   }

   std::size_t priv_get_cache_bucket_num()
   {  return this->cached_begin_ - this->internal.internal.priv_bucket_pointer();  }

   void priv_initialize_cache()
   {  this->cached_begin_ = this->internal.internal.priv_invalid_bucket();  }

   void priv_swap_cache(bucket_hash_equal_t &other)
   {
      std::swap(this->cached_begin_, other.cached_begin_);
   }

   siterator priv_begin() const
   {
      if(this->cached_begin_ == this->internal.internal.priv_invalid_bucket()){
         return this->internal.internal.priv_invalid_local_it();
      }
      else{
         return this->cached_begin_->begin();
      }
   }

   void priv_insertion_update_cache(size_type insertion_bucket)
   {
      bucket_ptr p = this->internal.internal.priv_bucket_pointer() + insertion_bucket;
      if(p < this->cached_begin_){
         this->cached_begin_ = p;
      }
   }

   const value_equal &priv_equal() const
   {  return this->detail::ebo_functor_holder<value_equal>::get();  }

   value_equal &priv_equal()
   {  return this->detail::ebo_functor_holder<value_equal>::get();  }

   void priv_erasure_update_cache_range(size_type first_bucket_num, size_type last_bucket_num)
   {
      //If the last bucket is the end, the cache must be updated
      //to the last position if all
      if(this->priv_get_cache_bucket_num() == first_bucket_num   &&
         this->internal.internal.priv_bucket_pointer()[first_bucket_num].empty()          ){
         this->priv_set_cache(this->internal.internal.priv_bucket_pointer() + last_bucket_num);
         this->priv_erasure_update_cache();
      }
   }

   void priv_erasure_update_cache()
   {
      if(this->cached_begin_ != this->internal.internal.priv_invalid_bucket()){
         size_type current_n = this->priv_get_cache() - this->internal.internal.priv_bucket_pointer();
         for( const size_type num_buckets = this->internal.internal.priv_bucket_count()
            ; current_n < num_buckets
            ; ++current_n, ++this->priv_get_cache()){
            if(!this->priv_get_cache()->empty()){
               return;
            }
         }
         this->priv_initialize_cache();
      }
   }

   bucket_ptr cached_begin_;
   bucket_hash_t<VoidOrKeyHash, ValueTraits, BucketTraits> internal; //2
};

//hashdata_internal
//Stores bucket_hash_equal_t and split_traits
template<class SizeType, std::size_t BoolFlags, class VoidOrKeyHash, class VoidOrKeyEqual, class ValueTraits, class BucketTraits>
struct hashdata_internal
   : public detail::size_holder< 0 != (BoolFlags & hash_bool_flags::incremental_pos), SizeType, int>   //split_traits
{
   typedef bucket_hash_equal_t
      < VoidOrKeyHash, VoidOrKeyEqual
      , ValueTraits, BucketTraits
      , 0 != (BoolFlags & hash_bool_flags::cache_begin_pos)
      >                                                     internal_type;
   typedef typename internal_type::value_equal              value_equal;
   typedef typename internal_type::hasher                   hasher;
   typedef bucket_plus_vtraits<ValueTraits,BucketTraits>    bucket_plus_vtraits_t;
   typedef typename bucket_plus_vtraits_t::size_type        size_type;
   typedef typename bucket_plus_vtraits_t::bucket_ptr       bucket_ptr;
   typedef detail::size_holder
      <0 != (BoolFlags & hash_bool_flags::incremental_pos)
      , SizeType, int>                                      split_traits;
   typedef typename bucket_plus_vtraits_t::
      value_traits::node_traits                             node_traits;
   typedef detail::bool_<detail::optimize_multikey_is_true
      <node_traits>::value>                                 optimize_multikey_t;

   template<class BucketTraitsType>
   hashdata_internal( const ValueTraits &val_traits, BOOST_FWD_REF(BucketTraitsType) b_traits
                    , const hasher & h, const value_equal &e)
      :  internal(val_traits, ::boost::forward<BucketTraitsType>(b_traits), h, e)
   {}

   split_traits &priv_split_traits()
   {  return *this;  }

   const split_traits &priv_split_traits() const
   {  return *this;  }

   ~hashdata_internal()
   {  this->priv_clear_buckets();  }

   void priv_clear_buckets()
   {
      this->internal.internal.internal.priv_clear_buckets
         ( this->internal.priv_get_cache()
         , this->internal.internal.internal.priv_bucket_count()
            - (this->internal.priv_get_cache()
               - this->internal.internal.internal.priv_bucket_pointer()));
   }

   void priv_clear_buckets_and_cache()
   {
      this->priv_clear_buckets();
      this->internal.priv_initialize_cache();
   }

   void priv_initialize_buckets_and_cache()
   {
      this->internal.internal.internal.priv_clear_buckets
         ( this->internal.internal.internal.priv_bucket_pointer()
         , this->internal.internal.internal.priv_bucket_count());
      this->internal.priv_initialize_cache();
   }

   internal_type internal; //2
};

//hashtable_data_t
//Stores hashdata_internal and size_traits
template<class SizeType, std::size_t BoolFlags, class VoidOrKeyHash, class VoidOrKeyEqual, class ValueTraits, class BucketTraits>
struct hashtable_data_t
   : public detail::size_holder
      < 0 != (BoolFlags & hash_bool_flags::constant_time_size_pos), SizeType> //size_traits
{
   typedef detail::size_holder
      < 0 != (BoolFlags & hash_bool_flags::constant_time_size_pos)
      , SizeType>                                              size_traits;
   typedef hashdata_internal
      < SizeType
      , BoolFlags & (hash_bool_flags::incremental_pos | hash_bool_flags::cache_begin_pos)
      , VoidOrKeyHash, VoidOrKeyEqual
      , ValueTraits, BucketTraits>                             internal_type;
   typedef ValueTraits                                         value_traits;
   typedef typename internal_type::value_equal                 value_equal;
   typedef typename internal_type::hasher                      hasher;
   typedef BucketTraits                                        bucket_traits;
   typedef bucket_plus_vtraits
      <ValueTraits,BucketTraits>                               bucket_plus_vtraits_t;

   template<class BucketTraitsType>
   hashtable_data_t( BOOST_FWD_REF(BucketTraitsType) b_traits, const hasher & h
                   , const value_equal &e, const value_traits &val_traits)
      : internal(val_traits, ::boost::forward<BucketTraitsType>(b_traits), h, e)
   {}

   internal_type internal; //1
};

/// @endcond

//! The class template hashtable is an intrusive hash table container, that
//! is used to construct intrusive unordered_set and unordered_multiset containers. The
//! no-throw guarantee holds only, if the VoidOrKeyEqual object and Hasher don't throw.
//!
//! hashtable is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: hashtable needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! hashtable more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, power_2_buckets<>, cache_begin<> and incremental<>.
//!
//! hashtable only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size hashtables because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant_time size hashtables are mainly provided to support auto-unlink hooks.
//!
//! hashtables, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehashing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidOrKeyHash, class VoidOrKeyEqual, class SizeType, class BucketTraits, std::size_t BoolFlags>
#endif
class hashtable_impl
   :  private hashtable_data_t
      < SizeType
      , BoolFlags & hashtable_data_bool_flags_mask
      , VoidOrKeyHash, VoidOrKeyEqual, ValueTraits, BucketTraits>
{
   typedef hashtable_data_t
      < SizeType
      , BoolFlags & hashtable_data_bool_flags_mask
      , VoidOrKeyHash, VoidOrKeyEqual, ValueTraits, BucketTraits> data_type;

   public:
   typedef ValueTraits  value_traits;

   /// @cond
   typedef BucketTraits                                              bucket_traits;

   typedef typename detail::get_slist_impl
      <typename detail::reduced_slist_node_traits
         <typename value_traits::node_traits>::type
      >::type                                                        slist_impl;
   typedef bucket_plus_vtraits<ValueTraits, BucketTraits>            bucket_plus_vtraits_t;
   typedef typename bucket_plus_vtraits_t::const_value_traits_ptr    const_value_traits_ptr;


   /// @endcond

   typedef typename value_traits::pointer                            pointer;
   typedef typename value_traits::const_pointer                      const_pointer;
   typedef typename value_traits::value_type                         value_type;
   typedef typename pointer_traits<pointer>::reference               reference;
   typedef typename pointer_traits<const_pointer>::reference         const_reference;
   typedef typename pointer_traits<pointer>::difference_type         difference_type;
   typedef SizeType                                                  size_type;
   typedef value_type                                                key_type;
   typedef typename data_type::value_equal                           key_equal;
   typedef typename data_type::value_equal                           value_equal;
   typedef typename data_type::hasher                                hasher;
   typedef detail::bucket_impl<slist_impl>                           bucket_type;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < bucket_type >::type                                       bucket_ptr;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < const bucket_type >::type                                 const_bucket_ptr;
   typedef typename pointer_traits
      <bucket_ptr>::reference                                        bucket_reference;
   typedef typename pointer_traits
      <bucket_ptr>::reference                                        const_bucket_reference;
   typedef typename slist_impl::iterator                             siterator;
   typedef typename slist_impl::const_iterator                       const_siterator;
   typedef hashtable_iterator<bucket_plus_vtraits_t, false>          iterator;
   typedef hashtable_iterator<bucket_plus_vtraits_t, true>           const_iterator;
   typedef typename value_traits::node_traits                        node_traits;
   typedef typename node_traits::node                                node;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < node >::type                                              node_ptr;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < const node >::type                                        const_node_ptr;
   typedef typename pointer_traits
      <node_ptr>::reference                                          node_reference;
   typedef typename pointer_traits
      <const_node_ptr>::reference                                    const_node_reference;
   typedef typename slist_impl::node_algorithms                      node_algorithms;

   static const bool stateful_value_traits = detail::is_stateful_value_traits<value_traits>::value;
   static const bool store_hash = detail::store_hash_is_true<node_traits>::value;

   static const bool unique_keys          = 0 != (BoolFlags & hash_bool_flags::unique_keys_pos);
   static const bool constant_time_size   = 0 != (BoolFlags & hash_bool_flags::constant_time_size_pos);
   static const bool cache_begin          = 0 != (BoolFlags & hash_bool_flags::cache_begin_pos);
   static const bool compare_hash         = 0 != (BoolFlags & hash_bool_flags::compare_hash_pos);
   static const bool incremental          = 0 != (BoolFlags & hash_bool_flags::incremental_pos);
   static const bool power_2_buckets      = incremental || (0 != (BoolFlags & hash_bool_flags::power_2_buckets_pos));

   static const bool optimize_multikey
      = detail::optimize_multikey_is_true<node_traits>::value && !unique_keys;

   /// @cond
   private:

   //Configuration error: compare_hash<> can't be specified without store_hash<>
   //See documentation for more explanations
   BOOST_STATIC_ASSERT((!compare_hash || store_hash));

   typedef typename slist_impl::node_ptr                             slist_node_ptr;
   typedef typename pointer_traits
      <slist_node_ptr>::template rebind_pointer
         < void >::type                                              void_pointer;
   //We'll define group traits, but these won't be instantiated if
   //optimize_multikey is not true
   typedef unordered_group_adapter<node_traits>                      group_traits;
   typedef circular_slist_algorithms<group_traits>                   group_algorithms;
   typedef detail::bool_<store_hash>                                 store_hash_t;
   typedef detail::bool_<optimize_multikey>                          optimize_multikey_t;
   typedef detail::bool_<cache_begin>                                cache_begin_t;
   typedef detail::bool_<power_2_buckets>                            power_2_buckets_t;
   typedef detail::size_holder<constant_time_size, size_type>        size_traits;
   typedef detail::size_holder<incremental, size_type, int>          split_traits;
   typedef detail::group_functions<node_traits>                      group_functions_t;
   typedef detail::node_functions<node_traits>                       node_functions_t;

   private:
   //noncopyable, movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(hashtable_impl)

   static const bool safemode_or_autounlink = is_safe_autounlink<value_traits::link_mode>::value;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_STATIC_ASSERT(!(constant_time_size && ((int)value_traits::link_mode == (int)auto_unlink)));
   //Cache begin is incompatible with auto-unlink hooks!
   BOOST_STATIC_ASSERT(!(cache_begin && ((int)value_traits::link_mode == (int)auto_unlink)));

   template<class Disposer>
   node_cast_adaptor< detail::node_disposer<Disposer, value_traits, CircularSListAlgorithms>
                    , slist_node_ptr, node_ptr >
      make_node_disposer(const Disposer &disposer) const
   {
      return node_cast_adaptor
         < detail::node_disposer<Disposer, value_traits, CircularSListAlgorithms>
         , slist_node_ptr, node_ptr >
            (disposer, &this->priv_value_traits());
   }

   /// @endcond

   public:
   typedef detail::insert_commit_data_impl insert_commit_data;

   typedef detail::transform_iterator
      < typename slist_impl::iterator
      , downcast_node_to_value_t
         < value_traits
         , false> >   local_iterator;

   typedef detail::transform_iterator
      < typename slist_impl::iterator
      , downcast_node_to_value_t 
         < value_traits
         , true> >    const_local_iterator;

   public:

   //! <b>Requires</b>: buckets must not be being used by any other resource.
   //!
   //! <b>Effects</b>: Constructs an empty unordered_set, storing a reference
   //!   to the bucket array and copies of the key_hasher and equal_func functors.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of hash_func or equal_func throws.
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed.
   explicit hashtable_impl ( const bucket_traits &b_traits
                           , const hasher & hash_func = hasher()
                           , const key_equal &equal_func = key_equal()
                           , const value_traits &v_traits = value_traits())
      :  data_type(b_traits, hash_func, equal_func, v_traits)
   {
      this->data_type::internal.priv_initialize_buckets_and_cache();
      this->priv_size_traits().set_size(size_type(0));
      size_type bucket_sz = this->priv_bucket_count();
      BOOST_INTRUSIVE_INVARIANT_ASSERT(bucket_sz != 0);
      //Check power of two bucket array if the option is activated
      BOOST_INTRUSIVE_INVARIANT_ASSERT
         (!power_2_buckets || (0 == (bucket_sz & (bucket_sz-1))));
      this->priv_split_traits().set_size(bucket_sz>>1);
   }

   //! <b>Effects</b>: to-do
   //!
   hashtable_impl(BOOST_RV_REF(hashtable_impl) x)
      : data_type( ::boost::move(x.priv_bucket_traits())
             , ::boost::move(x.priv_hasher())
             , ::boost::move(x.priv_equal())
             , ::boost::move(x.priv_value_traits())
             )
   {
      this->priv_swap_cache(x);
      x.priv_initialize_cache();
      if(constant_time_size){
         this->priv_size_traits().set_size(size_type(0));
         this->priv_size_traits().set_size(x.priv_size_traits().get_size());
         x.priv_size_traits().set_size(size_type(0));
      }
      if(incremental){
         this->priv_split_traits().set_size(x.priv_split_traits().get_size());
         x.priv_split_traits().set_size(size_type(0));
      }
   }

   //! <b>Effects</b>: to-do
   //!
   hashtable_impl& operator=(BOOST_RV_REF(hashtable_impl) x)
   {  this->swap(x); return *this;  }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Detaches all elements from this. The objects in the unordered_set
   //!   are not deleted (i.e. no destructors are called).
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the unordered_set, if
   //!   it's a safe-mode or auto-unlink value. Otherwise constant.
   //!
   //! <b>Throws</b>: Nothing.
   ~hashtable_impl();
   #endif

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   iterator begin()
   {  return iterator(this->priv_begin(), &this->get_bucket_value_traits());   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   {  return this->cbegin();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   {  return const_iterator(this->priv_begin(), &this->get_bucket_value_traits());   }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator end()
   {  return iterator(this->priv_invalid_local_it(), 0);   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   {  return this->cend(); }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   {  return const_iterator(this->priv_invalid_local_it(), 0);  }

   //! <b>Effects</b>: Returns the hasher object used by the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If hasher copy-constructor throws.
   hasher hash_function() const
   {  return this->priv_hasher();  }

   //! <b>Effects</b>: Returns the key_equal object used by the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If key_equal copy-constructor throws.
   key_equal key_eq() const
   {  return this->priv_equal();   }

   //! <b>Effects</b>: Returns true if the container is empty.
   //!
   //! <b>Complexity</b>: if constant-time size and cache_begin options are disabled,
   //!   average constant time (worst case, with empty() == true: O(this->bucket_count()).
   //!   Otherwise constant.
   //!
   //! <b>Throws</b>: Nothing.
   bool empty() const
   {
      if(constant_time_size){
         return !this->size();
      }
      else if(cache_begin){
         return this->begin() == this->end();
      }
      else{
         size_type bucket_cnt = this->priv_bucket_count();
         const bucket_type *b = boost::intrusive::detail::to_raw_pointer(this->priv_bucket_pointer());
         for (size_type n = 0; n < bucket_cnt; ++n, ++b){
            if(!b->empty()){
               return false;
            }
         }
         return true;
      }
   }

   //! <b>Effects</b>: Returns the number of elements stored in the unordered_set.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this if
   //!   constant_time_size is false. Constant-time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   size_type size() const
   {
      if(constant_time_size)
         return this->priv_size_traits().get_size();
      else{
         size_type len = 0;
         size_type bucket_cnt = this->priv_bucket_count();
         const bucket_type *b = boost::intrusive::detail::to_raw_pointer(this->priv_bucket_pointer());
         for (size_type n = 0; n < bucket_cnt; ++n, ++b){
            len += b->size();
         }
         return len;
      }
   }

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
   void swap(hashtable_impl& other)
   {
      using std::swap;
      //These can throw
      swap(this->priv_equal(),  other.priv_equal());
      swap(this->priv_hasher(), other.priv_hasher());
      //These can't throw
      swap(this->priv_bucket_traits(), other.priv_bucket_traits());
      swap(this->priv_value_traits(), other.priv_value_traits());
      this->priv_swap_cache(other);
      if(constant_time_size){
         size_type backup = this->priv_size_traits().get_size();
         this->priv_size_traits().set_size(other.priv_size_traits().get_size());
         other.priv_size_traits().set_size(backup);
      }
      if(incremental){
         size_type backup = this->priv_split_traits().get_size();
         this->priv_split_traits().set_size(other.priv_split_traits().get_size());
         other.priv_split_traits().set_size(backup);
      }
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw
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
   void clone_from(const hashtable_impl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!constant_time_size || !src.empty()){
         const size_type src_bucket_count = src.bucket_count();
         const size_type dst_bucket_count = this->bucket_count();
         //Check power of two bucket array if the option is activated
         BOOST_INTRUSIVE_INVARIANT_ASSERT
         (!power_2_buckets || (0 == (src_bucket_count & (src_bucket_count-1))));
         BOOST_INTRUSIVE_INVARIANT_ASSERT
         (!power_2_buckets || (0 == (dst_bucket_count & (dst_bucket_count-1))));

         //If src bucket count is bigger or equal, structural copy is possible
         if(!incremental && (src_bucket_count >= dst_bucket_count)){
            //First clone the first ones
            const bucket_ptr src_buckets = src.priv_bucket_pointer();
            const bucket_ptr dst_buckets = this->priv_bucket_pointer();
            size_type constructed;
                                      
            typedef node_cast_adaptor< detail::node_disposer<Disposer, value_traits, CircularSListAlgorithms>
                                     , slist_node_ptr, node_ptr > NodeDisposer;
            typedef node_cast_adaptor< detail::node_cloner  <Cloner,   value_traits, CircularSListAlgorithms>
                                     , slist_node_ptr, node_ptr > NodeCloner;
            NodeDisposer node_disp(disposer, &this->priv_value_traits());

            detail::exception_array_disposer<bucket_type, NodeDisposer, size_type>
               rollback(dst_buckets[0], node_disp, constructed);
            for( constructed = 0
               ; constructed < dst_bucket_count
               ; ++constructed){
               dst_buckets[constructed].clone_from
                  ( src_buckets[constructed]
                  , NodeCloner(cloner, &this->priv_value_traits()), node_disp);
            }
            if(src_bucket_count != dst_bucket_count){
               //Now insert the remaining ones using the modulo trick
               for(//"constructed" comes from the previous loop
                  ; constructed < src_bucket_count
                  ; ++constructed){
                  bucket_type &dst_b =
                     dst_buckets[detail::hash_to_bucket_split<power_2_buckets, incremental>(constructed, dst_bucket_count, dst_bucket_count)];
                  bucket_type &src_b = src_buckets[constructed];
                  for( siterator b(src_b.begin()), e(src_b.end())
                     ; b != e
                     ; ++b){
                     dst_b.push_front(*(NodeCloner(cloner, &this->priv_value_traits())(*b.pointed_node())));
                  }
               }
            }
            this->priv_hasher() = src.priv_hasher();
            this->priv_equal()  = src.priv_equal();
            rollback.release();
            this->priv_size_traits().set_size(src.priv_size_traits().get_size());
            this->priv_split_traits().set_size(dst_bucket_count);
            this->priv_insertion_update_cache(0u);
            this->priv_erasure_update_cache();
         }
         else if(store_hash){
            //Unlike previous cloning algorithm, this can throw
            //if cloner, hasher or comparison functor throw
            const_iterator b(src.cbegin()), e(src.cend());
            detail::exception_disposer<hashtable_impl, Disposer>
               rollback(*this, disposer);
            for(; b != e; ++b){
               std::size_t hash_value = this->priv_stored_or_compute_hash(*b, store_hash_t());;
               this->priv_insert_equal_with_hash(*cloner(*b), hash_value);
            }
            rollback.release();
         }
         else{
            //Unlike previous cloning algorithm, this can throw
            //if cloner, hasher or comparison functor throw
            const_iterator b(src.cbegin()), e(src.cend());
            detail::exception_disposer<hashtable_impl, Disposer>
               rollback(*this, disposer);
            for(; b != e; ++b){
               this->insert_equal(*cloner(*b));
            }
            rollback.release();
         }
      }
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts the value into the unordered_set.
   //!
   //! <b>Returns</b>: An iterator to the inserted value.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(reference value)
   {
      size_type bucket_num;
      std::size_t hash_value;
      siterator prev;
      siterator it = this->priv_find
         (value, this->priv_hasher(), this->priv_equal(), bucket_num, hash_value, prev);
      return this->priv_insert_equal_find(value, bucket_num, hash_value, it);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Equivalent to this->insert_equal(t) for each element in [b, e).
   //!
   //! <b>Complexity</b>: Average case O(N), where N is std::distance(b, e).
   //!   Worst case O(N*this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      for (; b != e; ++b)
         this->insert_equal(*b);
   }

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
   std::pair<iterator, bool> insert_unique(reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = this->insert_unique_check
         (value, this->priv_hasher(), this->priv_equal(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<iterator, bool>
         (this->insert_unique_commit(value, commit_data), true);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Equivalent to this->insert_unique(t) for each element in [b, e).
   //!
   //! <b>Complexity</b>: Average case O(N), where N is std::distance(b, e).
   //!   Worst case O(N*this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      for (; b != e; ++b)
         this->insert_unique(*b);
   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
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
   //! <b>Throws</b>: If hash_func or equal_func throw. Strong guarantee.
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
   std::pair<iterator, bool> insert_unique_check
      ( const KeyType &key
      , KeyHasher hash_func
      , KeyValueEqual equal_func
      , insert_commit_data &commit_data)
   {
      size_type bucket_num;
      siterator prev;
      siterator prev_pos =
         this->priv_find(key, hash_func, equal_func, bucket_num, commit_data.hash, prev);
      bool success = prev_pos == this->priv_invalid_local_it();
      if(success){
         prev_pos = prev;
      }
      return std::pair<iterator, bool>(iterator(prev_pos, &this->get_bucket_value_traits()),success);
   }

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
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data)
   {
      size_type bucket_num = this->priv_hash_to_bucket(commit_data.hash);
      bucket_type &b = this->priv_bucket_pointer()[bucket_num];
      this->priv_size_traits().increment();
      node_ptr n = pointer_traits<node_ptr>::pointer_to(this->priv_value_to_node(value));
      node_functions_t::store_hash(n, commit_data.hash, store_hash_t());
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(n));
      this->priv_insertion_update_cache(bucket_num);
      group_functions_t::insert_in_group(node_ptr(), n, optimize_multikey_t());
      return iterator(b.insert_after(b.before_begin(), *n), &this->get_bucket_value_traits());
   }

   //! <b>Effects</b>: Erases the element pointed to by i.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased element. No destructors are called.
   void erase(const_iterator i)
   {  this->erase_and_dispose(i, detail::null_disposer());  }

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
   {  this->erase_and_dispose(b, e, detail::null_disposer());  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!   Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return this->erase(value, this->priv_hasher(), this->priv_equal());  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
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
   {  return this->erase_and_dispose(key, hash_func, equal_func, detail::null_disposer()); }

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
   {
      this->priv_erase(i, disposer, optimize_multikey_t());
      this->priv_size_traits().decrement();
      this->priv_erasure_update_cache();
   }

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
   {
      if(b != e){
         //Get the bucket number and local iterator for both iterators
         siterator first_local_it(b.slist_it());
         size_type first_bucket_num = this->priv_get_bucket_num(first_local_it);

         const bucket_ptr buck_ptr = this->priv_bucket_pointer();
         siterator before_first_local_it
            = this->priv_get_previous(buck_ptr[first_bucket_num], first_local_it);
         size_type last_bucket_num;
         siterator last_local_it;

         //For the end iterator, we will assign the end iterator
         //of the last bucket
         if(e == this->end()){
            last_bucket_num   = this->bucket_count() - 1;
            last_local_it     = buck_ptr[last_bucket_num].end();
         }
         else{
            last_local_it     = e.slist_it();
            last_bucket_num = this->priv_get_bucket_num(last_local_it);
         }
         this->priv_erase_range(before_first_local_it, first_bucket_num, last_local_it, last_bucket_num, disposer);
         this->priv_erasure_update_cache_range(first_bucket_num, last_bucket_num);
      }
   }

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
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!   Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {  return this->erase_and_dispose(value, this->priv_hasher(), this->priv_equal(), disposer);   }

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
   size_type erase_and_dispose(const KeyType& key, KeyHasher hash_func
                              ,KeyValueEqual equal_func, Disposer disposer)
   {
      size_type bucket_num;
      std::size_t h;
      siterator prev;
      siterator it = this->priv_find(key, hash_func, equal_func, bucket_num, h, prev);
      bool success = it != this->priv_invalid_local_it();
      size_type cnt(0);
      if(!success){
         return 0;
      }
      else if(optimize_multikey){
         siterator last = bucket_type::s_iterator_to
            (*node_traits::get_next(group_functions_t::get_last_in_group
               (detail::dcast_bucket_ptr<node>(it.pointed_node()), optimize_multikey_t())));
         this->priv_erase_range_impl(bucket_num, prev, last, disposer, cnt);
      }
      else{
         //If found erase all equal values
         bucket_type &b = this->priv_bucket_pointer()[bucket_num];
         for(siterator end_sit = b.end(); it != end_sit; ++cnt, ++it){
            slist_node_ptr n(it.pointed_node());
            const value_type &v = this->priv_value_from_slist_node(n);
            if(compare_hash){
               std::size_t vh = this->priv_stored_or_compute_hash(v, store_hash_t());
               if(h != vh || !equal_func(key, v)){
                  break;
               }
            }
            else if(!equal_func(key, v)){
               break;
            }
            this->priv_size_traits().decrement();
         }
         b.erase_after_and_dispose(prev, it, make_node_disposer(disposer));
      }
      this->priv_erasure_update_cache();
      return cnt;
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
      this->data_type::internal.priv_clear_buckets_and_cache();
      this->priv_size_traits().set_size(size_type(0));
   }

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
   {
      if(!constant_time_size || !this->empty()){
         size_type num_buckets = this->bucket_count();
         bucket_ptr b = this->priv_bucket_pointer();
         for(; num_buckets--; ++b){
            b->clear_and_dispose(make_node_disposer(disposer));
         }
         this->priv_size_traits().set_size(size_type(0));
      }
      this->priv_initialize_cache();
   }

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   size_type count(const_reference value) const
   {  return this->count(value, this->priv_hasher(), this->priv_equal());  }

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
   //! <b>Throws</b>: If hash_func or equal throw.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   size_type count(const KeyType &key, const KeyHasher &hash_func, const KeyValueEqual &equal_func) const
   {
      size_type bucket_n1, bucket_n2, cnt;
      this->priv_equal_range(key, hash_func, equal_func, bucket_n1, bucket_n2, cnt);
      return cnt;
   }

   //! <b>Effects</b>: Finds an iterator to the first element is equal to
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   iterator find(const_reference value)
   {  return this->find(value, this->priv_hasher(), this->priv_equal());   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   "key" according to the given hash and equality functor or end() if
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
   iterator find(const KeyType &key, KeyHasher hash_func, KeyValueEqual equal_func)
   {
      size_type bucket_n;
      std::size_t hash;
      siterator prev;
      siterator local_it = this->priv_find(key, hash_func, equal_func, bucket_n, hash, prev);
      return iterator(local_it, &this->get_bucket_value_traits());
   }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is
   //!   "key" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   const_iterator find(const_reference value) const
   {  return this->find(value, this->priv_hasher(), this->priv_equal());   }

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
   const_iterator find
      (const KeyType &key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {
      size_type bucket_n;
      std::size_t hash_value;
      siterator prev;
      siterator sit = this->priv_find(key, hash_func, equal_func, bucket_n, hash_value, prev);
      return const_iterator(sit, &this->get_bucket_value_traits());
   }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return this->equal_range(value, this->priv_hasher(), this->priv_equal());  }

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
   //! <b>Throws</b>: If hash_func or the equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<iterator,iterator> equal_range
      (const KeyType &key, KeyHasher hash_func, KeyValueEqual equal_func)
   {
      size_type bucket_n1, bucket_n2, cnt;
      std::pair<siterator, siterator> ret = this->priv_equal_range
         (key, hash_func, equal_func, bucket_n1, bucket_n2, cnt);
      return std::pair<iterator, iterator>
         (iterator(ret.first, &this->get_bucket_value_traits()), iterator(ret.second, &this->get_bucket_value_traits()));
   }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return this->equal_range(value, this->priv_hasher(), this->priv_equal());  }

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
   //! <b>Throws</b>: If the hasher or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<const_iterator,const_iterator> equal_range
      (const KeyType &key, KeyHasher hash_func, KeyValueEqual equal_func) const
   {
      size_type bucket_n1, bucket_n2, cnt;
      std::pair<siterator, siterator> ret =
         this->priv_equal_range(key, hash_func, equal_func, bucket_n1, bucket_n2, cnt);
      return std::pair<const_iterator, const_iterator>
         ( const_iterator(ret.first,  &this->get_bucket_value_traits())
         , const_iterator(ret.second, &this->get_bucket_value_traits()));
   }

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
   {
      return iterator(bucket_type::s_iterator_to
         (this->priv_value_to_node(value)), &this->get_bucket_value_traits());
   }

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
   {
      node_reference r = *pointer_traits<node_ptr>::const_cast_from
         (pointer_traits<const_node_ptr>::pointer_to(this->priv_value_to_node(value)));
      siterator sit = bucket_type::s_iterator_to(r);
      return const_iterator(sit, &this->get_bucket_value_traits());
   }

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
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      siterator sit = bucket_type::s_iterator_to(*value_traits::to_node_ptr(value));
      return local_iterator(sit, const_value_traits_ptr());
   }

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
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      node_reference r = *pointer_traits<node_ptr>::const_cast_from
         (value_traits::to_node_ptr(value));
      siterator sit = bucket_type::s_iterator_to(r);
      return const_local_iterator(sit, const_value_traits_ptr());
   }

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
   {
      siterator sit = bucket_type::s_iterator_to(this->priv_value_to_node(value));
      return local_iterator(sit, this->priv_value_traits_ptr());
   }

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
   {
      node_reference r = *pointer_traits<node_ptr>::const_cast_from
         (pointer_traits<const_node_ptr>::pointer_to(this->priv_value_to_node(value)));
      siterator sit = bucket_type::s_iterator_to(r);
      return const_local_iterator(sit, this->priv_value_traits_ptr());
   }

   //! <b>Effects</b>: Returns the number of buckets passed in the constructor
   //!   or the last rehash function.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   size_type bucket_count() const
   {  return this->priv_bucket_count();   }

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns the number of elements in the nth bucket.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   size_type bucket_size(size_type n) const
   {  return this->priv_bucket_pointer()[n].size();   }

   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the hash functor throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   size_type bucket(const key_type& k)  const
   {  return this->bucket(k, this->priv_hasher());   }

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
   size_type bucket(const KeyType& k, const KeyHasher &hash_func)  const
   {  return this->priv_hash_to_bucket(hash_func(k));   }

   //! <b>Effects</b>: Returns the bucket array pointer passed in the constructor
   //!   or the last rehash function.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   bucket_ptr bucket_pointer() const
   {  return this->priv_bucket_pointer();   }

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
   {  return local_iterator(this->priv_bucket_pointer()[n].begin(), this->priv_value_traits_ptr());  }

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
   {  return this->cbegin(n);  }

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
   {
      bucket_reference br = pointer_traits<bucket_ptr>::const_cast_from(this->priv_bucket_pointer())[n];
      return const_local_iterator(br.begin(), this->priv_value_traits_ptr());
   }

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
   {  return local_iterator(this->priv_bucket_pointer()[n].end(), this->priv_value_traits_ptr());  }

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
   {  return this->cend(n);  }

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
   {
      bucket_reference br = pointer_traits<bucket_ptr>::const_cast_from(this->priv_bucket_pointer())[n];
      return const_local_iterator ( br.end(), this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: new_bucket_traits can hold a pointer to a new bucket array
   //!   or the same as the old bucket array with a different length. new_size is the length of the
   //!   the array pointed by new_buckets. If new_bucket_traits.bucket_begin() == this->bucket_pointer()
   //!   new_bucket_traits.bucket_count() can be bigger or smaller than this->bucket_count().
   //!   'new_bucket_traits' copy constructor should not throw.
   //!
   //! <b>Effects</b>: Updates the internal reference with the new bucket, erases
   //!   the values from the old bucket and inserts then in the new one.
   //!   Bucket traits hold by *this is assigned from new_bucket_traits.
   //!   If the container is configured as incremental<>, the split bucket is set
   //!   to the new bucket_count().
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!
   //! <b>Complexity</b>: Average case linear in this->size(), worst case quadratic.
   //!
   //! <b>Throws</b>: If the hasher functor throws. Basic guarantee.
   void rehash(const bucket_traits &new_bucket_traits)
   {
      const bucket_ptr new_buckets      = new_bucket_traits.bucket_begin();
            size_type  new_bucket_count = new_bucket_traits.bucket_count();
      const bucket_ptr old_buckets      = this->priv_bucket_pointer();
            size_type  old_bucket_count = this->priv_bucket_count();

      //Check power of two bucket array if the option is activated
      BOOST_INTRUSIVE_INVARIANT_ASSERT
      (!power_2_buckets || (0 == (new_bucket_count & (new_bucket_count-1u))));

      size_type n = this->priv_get_cache_bucket_num();
      const bool same_buffer = old_buckets == new_buckets;
      //If the new bucket length is a common factor
      //of the old one we can avoid hash calculations.
      const bool fast_shrink = (!incremental) && (old_bucket_count > new_bucket_count) &&
         (power_2_buckets ||(old_bucket_count % new_bucket_count) == 0);
      //If we are shrinking the same bucket array and it's
      //is a fast shrink, just rehash the last nodes
      size_type new_first_bucket_num = new_bucket_count;
      if(same_buffer && fast_shrink && (n < new_bucket_count)){
         n = new_bucket_count;
         new_first_bucket_num = this->priv_get_cache_bucket_num();
      }

      //Anti-exception stuff: they destroy the elements if something goes wrong.
      //If the source and destination buckets are the same, the second rollback function
      //is harmless, because all elements have been already unlinked and destroyed
      typedef detail::init_disposer<node_algorithms> NodeDisposer;
      NodeDisposer node_disp;
      bucket_type & newbuck = new_buckets[0];
      bucket_type & oldbuck = old_buckets[0];
      detail::exception_array_disposer<bucket_type, NodeDisposer, size_type>
         rollback1(newbuck, node_disp, new_bucket_count);
      detail::exception_array_disposer<bucket_type, NodeDisposer, size_type>
         rollback2(oldbuck, node_disp, old_bucket_count);

      //Put size in a safe value for rollback exception
      size_type size_backup = this->priv_size_traits().get_size();
      this->priv_size_traits().set_size(0);
      //Put cache to safe position
      this->priv_initialize_cache();
      this->priv_insertion_update_cache(size_type(0u));

      //Iterate through nodes
      for(; n < old_bucket_count; ++n){
         bucket_type &old_bucket = old_buckets[n];

         if(!fast_shrink){
            siterator before_i(old_bucket.before_begin());
            siterator end_sit(old_bucket.end());
            siterator i(old_bucket.begin());
            for(;i != end_sit; ++i){
               const value_type &v = this->priv_value_from_slist_node(i.pointed_node());
               const std::size_t hash_value = this->priv_stored_or_compute_hash(v, store_hash_t());
               const size_type new_n = detail::hash_to_bucket_split<power_2_buckets, incremental>(hash_value, new_bucket_count, new_bucket_count);
               if(cache_begin && new_n < new_first_bucket_num)
                  new_first_bucket_num = new_n;
               siterator last = bucket_type::s_iterator_to
                  (*group_functions_t::get_last_in_group
                     (detail::dcast_bucket_ptr<node>(i.pointed_node()), optimize_multikey_t()));
               if(same_buffer && new_n == n){
                  before_i = last;
               }
               else{
                  bucket_type &new_b = new_buckets[new_n];
                  new_b.splice_after(new_b.before_begin(), old_bucket, before_i, last);
               }
               i = before_i;
            }
         }
         else{
            const size_type new_n = detail::hash_to_bucket_split<power_2_buckets, incremental>(n, new_bucket_count, new_bucket_count);
            if(cache_begin && new_n < new_first_bucket_num)
               new_first_bucket_num = new_n;
            bucket_type &new_b = new_buckets[new_n];
            if(!old_bucket.empty()){
               new_b.splice_after( new_b.before_begin()
                                 , old_bucket
                                 , old_bucket.before_begin()
                                 , hashtable_impl::priv_get_last(old_bucket));
            }
         }
      }

      this->priv_size_traits().set_size(size_backup);
      this->priv_split_traits().set_size(new_bucket_count);
      this->priv_bucket_traits() = new_bucket_traits;
      this->priv_initialize_cache();
      this->priv_insertion_update_cache(new_first_bucket_num);
      rollback1.release();
      rollback2.release();
   }

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
   {
      //This function is only available for containers with incremental hashing
      BOOST_STATIC_ASSERT(( incremental && power_2_buckets ));
      const size_type split_idx  = this->priv_split_traits().get_size();
      const size_type bucket_cnt = this->priv_bucket_count();
      const bucket_ptr buck_ptr  = this->priv_bucket_pointer();

      if(grow){
         //Test if the split variable can be changed
         if(split_idx >= bucket_cnt)
            return false;

         const size_type bucket_to_rehash = split_idx - bucket_cnt/2;
         bucket_type &old_bucket = buck_ptr[bucket_to_rehash];
         siterator before_i(old_bucket.before_begin());
         const siterator end_sit(old_bucket.end());
         siterator i(old_bucket.begin());
         this->priv_split_traits().increment();

         //Anti-exception stuff: if an exception is thrown while
         //moving elements from old_bucket to the target bucket, all moved
         //elements are moved back to the original one.
         detail::incremental_rehash_rollback<bucket_type, split_traits> rollback
            ( buck_ptr[split_idx], old_bucket, this->priv_split_traits());
         for(;i != end_sit; ++i){
            const value_type &v = this->priv_value_from_slist_node(i.pointed_node());
            const std::size_t hash_value = this->priv_stored_or_compute_hash(v, store_hash_t());
            const size_type new_n = this->priv_hash_to_bucket(hash_value);
            siterator last = bucket_type::s_iterator_to
               (*group_functions_t::get_last_in_group
                  (detail::dcast_bucket_ptr<node>(i.pointed_node()), optimize_multikey_t()));
            if(new_n == bucket_to_rehash){
               before_i = last;
            }
            else{
               bucket_type &new_b = buck_ptr[new_n];
               new_b.splice_after(new_b.before_begin(), old_bucket, before_i, last);
            }
            i = before_i;
         }
         rollback.release();
         this->priv_erasure_update_cache();
         return true;
      }
      else{
         //Test if the split variable can be changed
         if(split_idx <= bucket_cnt/2)
            return false;
         const size_type target_bucket_num = split_idx - 1 - bucket_cnt/2;
         bucket_type &target_bucket = buck_ptr[target_bucket_num];
         bucket_type &source_bucket = buck_ptr[split_idx-1];
         target_bucket.splice_after(target_bucket.cbefore_begin(), source_bucket);
         this->priv_split_traits().decrement();
         this->priv_insertion_update_cache(target_bucket_num);
         return true;
      }
   }

   //! <b>Effects</b>: If new_bucket_traits.bucket_count() is not
   //!   this->bucket_count()/2 or this->bucket_count()*2, or
   //!   this->split_bucket() != new_bucket_traits.bucket_count() returns false
   //!   and does nothing.
   //!
   //!   Otherwise, copy assigns new_bucket_traits to the internal bucket_traits
   //!   and transfers all the objects from old buckets to the new ones.
   //!
   //! <b>Complexity</b>: Linear to size().
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(const bucket_traits &new_bucket_traits)
   {
      //This function is only available for containers with incremental hashing
      BOOST_STATIC_ASSERT(( incremental && power_2_buckets ));
      size_type new_bucket_traits_size = new_bucket_traits.bucket_count();
      size_type cur_bucket_traits      = this->priv_bucket_count();
      if(new_bucket_traits_size/2 != cur_bucket_traits && new_bucket_traits_size != cur_bucket_traits/2){
         return false;
      }

      const size_type split_idx = this->split_count();

      if(new_bucket_traits_size/2 == cur_bucket_traits){
         //Test if the split variable can be changed
         if(!(split_idx >= cur_bucket_traits))
            return false;
      }
      else{
         //Test if the split variable can be changed
         if(!(split_idx <= cur_bucket_traits/2))
            return false;
      }

      const size_type ini_n = this->priv_get_cache_bucket_num();
      const bucket_ptr old_buckets = this->priv_bucket_pointer();
      this->priv_bucket_traits() = new_bucket_traits;
      if(new_bucket_traits.bucket_begin() != old_buckets){
         for(size_type n = ini_n; n < split_idx; ++n){
            bucket_type &new_bucket = new_bucket_traits.bucket_begin()[n];
            bucket_type &old_bucket = old_buckets[n];
            new_bucket.splice_after(new_bucket.cbefore_begin(), old_bucket);
         }
         //Put cache to safe position
         this->priv_initialize_cache();
         this->priv_insertion_update_cache(ini_n);
      }
      return true;
   }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>:
   //!
   //! <b>Complexity</b>:
   //!
   //! <b>Throws</b>:
   size_type split_count() const
   {
      //This function is only available if incremental hashing is activated
      BOOST_STATIC_ASSERT(( incremental && power_2_buckets ));
      return this->priv_split_traits().get_size();
   }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is bigger or equal than n. This suggestion can be
   //!   used to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the
   //!   higher possible value is returned.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static size_type suggested_upper_bucket_count(size_type n)
   {
      const std::size_t *primes     = &detail::prime_list_holder<0>::prime_list[0];
      const std::size_t *primes_end = primes + detail::prime_list_holder<0>::prime_list_size;
      std::size_t const* bound = std::lower_bound(primes, primes_end, n);
      bound -= (bound == primes_end);
      return size_type(*bound);
   }

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is smaller or equal than n. This suggestion can be
   //!   used to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the
   //!   lowest possible value is returned.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static size_type suggested_lower_bucket_count(size_type n)
   {
      const std::size_t *primes     = &detail::prime_list_holder<0>::prime_list[0];
      const std::size_t *primes_end = primes + detail::prime_list_holder<0>::prime_list_size;
      size_type const* bound = std::upper_bound(primes, primes_end, n);
      bound -= (bound != primes);
      return size_type(*bound);
   }
   /// @cond
   private:
   size_traits &priv_size_traits()
   {  return static_cast<size_traits&>(static_cast<data_type&>(*this));  }

   const size_traits &priv_size_traits() const
   {  return static_cast<const size_traits&>(static_cast<const data_type&>(*this));  }

   bucket_ptr priv_bucket_pointer() const
   {  return this->data_type::internal.internal.internal.internal.priv_bucket_pointer();  }

   SizeType priv_bucket_count() const
   {  return this->data_type::internal.internal.internal.internal.priv_bucket_count();  }

   const bucket_plus_vtraits<ValueTraits, BucketTraits> &get_bucket_value_traits() const
   {  return this->data_type::internal.internal.internal.internal.get_bucket_value_traits(); }

   bucket_plus_vtraits<ValueTraits, BucketTraits> &get_bucket_value_traits()
   {  return this->data_type::internal.internal.internal.internal.get_bucket_value_traits(); }

   bucket_traits &priv_bucket_traits()
   {  return this->data_type::internal.internal.internal.internal.priv_bucket_traits(); }

   const bucket_traits &priv_bucket_traits() const
   {  return this->data_type::internal.internal.internal.internal.priv_bucket_traits(); }

   value_traits &priv_value_traits()
   {  return this->data_type::internal.internal.internal.internal.priv_value_traits(); }

   const value_traits &priv_value_traits() const
   {  return this->data_type::internal.internal.internal.internal.priv_value_traits(); }

   const_value_traits_ptr priv_value_traits_ptr() const
   {  return this->data_type::internal.internal.internal.internal.priv_value_traits_ptr(); }

   siterator priv_invalid_local_it() const
   {  return this->data_type::internal.internal.internal.internal.priv_invalid_local_it(); }
   
   split_traits &priv_split_traits()
   {  return this->data_type::internal.priv_split_traits();  }

   const split_traits &priv_split_traits() const
   {  return this->data_type::internal.priv_split_traits();  }

   bucket_ptr priv_get_cache()
   {  return this->data_type::internal.internal.priv_get_cache();  }

   void priv_initialize_cache()
   {  return this->data_type::internal.internal.priv_initialize_cache();  }

   siterator priv_begin() const
   {  return this->data_type::internal.internal.priv_begin(); }

   const value_equal &priv_equal() const
   {  return this->data_type::internal.internal.priv_equal(); }

   value_equal &priv_equal()
   {  return this->data_type::internal.internal.priv_equal(); }

   const hasher &priv_hasher() const
   {  return this->data_type::internal.internal.internal.priv_hasher(); }

   hasher &priv_hasher()
   {  return this->data_type::internal.internal.internal.priv_hasher(); }

   void priv_swap_cache(hashtable_impl &h)
   {  this->data_type::internal.internal.priv_swap_cache(h.data_type::internal.internal);   }

   node &priv_value_to_node(value_type &v)
   {  return this->data_type::internal.internal.internal.internal.priv_value_to_node(v); }

   const node &priv_value_to_node(const value_type &v) const
   {  return this->data_type::internal.internal.internal.internal.priv_value_to_node(v); }

   SizeType priv_get_cache_bucket_num()
   {  return this->data_type::internal.internal.priv_get_cache_bucket_num(); }

   void priv_insertion_update_cache(SizeType n)
   {  return this->data_type::internal.internal.priv_insertion_update_cache(n); }

   template<bool Boolean>
   std::size_t priv_stored_or_compute_hash(const value_type &v, detail::bool_<Boolean> b) const
   {  return this->data_type::internal.internal.internal.priv_stored_or_compute_hash(v, b); }

   value_type &priv_value_from_slist_node(slist_node_ptr n)
   {  return this->data_type::internal.internal.internal.internal.priv_value_from_slist_node(n); }

   const value_type &priv_value_from_slist_node(slist_node_ptr n) const
   {  return this->data_type::internal.internal.internal.internal.priv_value_from_slist_node(n); }

   void priv_erasure_update_cache_range(SizeType first_bucket_num, SizeType last_bucket_num)
   {  return this->data_type::internal.internal.priv_erasure_update_cache_range(first_bucket_num, last_bucket_num); }

   void priv_erasure_update_cache()
   {  return this->data_type::internal.internal.priv_erasure_update_cache(); }

   static std::size_t priv_stored_hash(slist_node_ptr n, detail::true_ true_value)
   {  return bucket_plus_vtraits<ValueTraits, BucketTraits>::priv_stored_hash(n, true_value); }

   static std::size_t priv_stored_hash(slist_node_ptr n, detail::false_ false_value)
   {  return bucket_plus_vtraits<ValueTraits, BucketTraits>::priv_stored_hash(n, false_value); }

   std::size_t priv_hash_to_bucket(std::size_t hash_value) const
   {
      return detail::hash_to_bucket_split<power_2_buckets, incremental>
         (hash_value, this->priv_bucket_traits().bucket_count(), this->priv_split_traits().get_size());
   }

   template<class Disposer>
   void priv_erase_range_impl
      (size_type bucket_num, siterator before_first_it, siterator end_sit, Disposer disposer, size_type &num_erased)
   {
      const bucket_ptr buckets = this->priv_bucket_pointer();
      bucket_type &b = buckets[bucket_num];

      if(before_first_it == b.before_begin() && end_sit == b.end()){
         this->priv_erase_range_impl(bucket_num, 1, disposer, num_erased);
      }
      else{
         num_erased = 0;
         siterator to_erase(before_first_it);
         ++to_erase;
         slist_node_ptr end_ptr = end_sit.pointed_node();
         while(to_erase != end_sit){
            group_functions_t::erase_from_group(end_ptr, detail::dcast_bucket_ptr<node>(to_erase.pointed_node()), optimize_multikey_t());
            to_erase = b.erase_after_and_dispose(before_first_it, make_node_disposer(disposer));
            ++num_erased;
         }
         this->priv_size_traits().set_size(this->priv_size_traits().get_size()-num_erased);
      }
   }

   template<class Disposer>
   void priv_erase_range_impl
      (size_type first_bucket_num, size_type num_buckets, Disposer disposer, size_type &num_erased)
   {
      //Now fully clear the intermediate buckets
      const bucket_ptr buckets = this->priv_bucket_pointer();
      num_erased = 0;
      for(size_type i = first_bucket_num; i < (num_buckets + first_bucket_num); ++i){
         bucket_type &b = buckets[i];
         siterator b_begin(b.before_begin());
         siterator nxt(b_begin);
         ++nxt;
         siterator end_sit(b.end());
         while(nxt != end_sit){
            group_functions_t::init_group(detail::dcast_bucket_ptr<node>(nxt.pointed_node()), optimize_multikey_t());
            nxt = b.erase_after_and_dispose
               (b_begin, make_node_disposer(disposer));
            this->priv_size_traits().decrement();
            ++num_erased;
         }
      }
   }

   template<class Disposer>
   void priv_erase_range( siterator before_first_it,  size_type first_bucket
                        , siterator last_it,          size_type last_bucket
                        , Disposer disposer)
   {
      size_type num_erased;
      if (first_bucket == last_bucket){
         this->priv_erase_range_impl(first_bucket, before_first_it, last_it, disposer, num_erased);
      }
      else {
         bucket_type *b = (&this->priv_bucket_pointer()[0]);
         this->priv_erase_range_impl(first_bucket, before_first_it, b[first_bucket].end(), disposer, num_erased);
         if(size_type n = (last_bucket - first_bucket - 1))
            this->priv_erase_range_impl(first_bucket + 1, n, disposer, num_erased);
         this->priv_erase_range_impl(last_bucket, b[last_bucket].before_begin(), last_it, disposer, num_erased);
      }
   }

   std::size_t priv_get_bucket_num(siterator it)
   {  return this->priv_get_bucket_num_hash_dispatch(it, store_hash_t());  }

   std::size_t priv_get_bucket_num_hash_dispatch(siterator it, detail::true_)    //store_hash
   {
      return this->priv_hash_to_bucket
         (this->priv_stored_hash(it.pointed_node(), store_hash_t()));
   }

   std::size_t priv_get_bucket_num_hash_dispatch(siterator it, detail::false_)   //NO store_hash
   {  return this->data_type::internal.internal.internal.internal.priv_get_bucket_num_no_hash_store(it, optimize_multikey_t());  }

   static siterator priv_get_previous(bucket_type &b, siterator i)
   {  return bucket_plus_vtraits_t::priv_get_previous(b, i, optimize_multikey_t());   }

   static siterator priv_get_last(bucket_type &b)
   {  return bucket_plus_vtraits_t::priv_get_last(b, optimize_multikey_t());  }

   template<class Disposer>
   void priv_erase(const_iterator i, Disposer disposer, detail::true_)
   {
      slist_node_ptr elem(i.slist_it().pointed_node());
      slist_node_ptr f_bucket_end, l_bucket_end;
      if(store_hash){
         f_bucket_end = l_bucket_end =
         (this->priv_bucket_pointer()
            [this->priv_hash_to_bucket
               (this->priv_stored_hash(elem, store_hash_t()))
            ]).before_begin().pointed_node();
      }
      else{
         f_bucket_end = this->priv_bucket_pointer()->cend().pointed_node();
         l_bucket_end = f_bucket_end + this->priv_bucket_count() - 1;
      }
      node_ptr nxt_in_group;
      siterator prev = bucket_type::s_iterator_to
         (*group_functions_t::get_previous_and_next_in_group
            ( elem, nxt_in_group, f_bucket_end, l_bucket_end)
         );
      bucket_type::s_erase_after_and_dispose(prev, make_node_disposer(disposer));
      if(nxt_in_group)
         group_algorithms::unlink_after(nxt_in_group);
      if(safemode_or_autounlink)
         group_algorithms::init(detail::dcast_bucket_ptr<node>(elem));
   }

   template <class Disposer>
   void priv_erase(const_iterator i, Disposer disposer, detail::false_)
   {
      siterator to_erase(i.slist_it());
      bucket_type &b = this->priv_bucket_pointer()[this->priv_get_bucket_num(to_erase)];
      siterator prev(this->priv_get_previous(b, to_erase));
      b.erase_after_and_dispose(prev, make_node_disposer(disposer));
   }

   template<class KeyType, class KeyHasher, class KeyValueEqual>
   siterator priv_find
      ( const KeyType &key,  KeyHasher hash_func
      , KeyValueEqual equal_func, size_type &bucket_number, std::size_t &h, siterator &previt) const
   {
      h = hash_func(key);
      return this->priv_find_with_hash(key, equal_func, bucket_number, h, previt);
   }

   template<class KeyType, class KeyValueEqual>
   siterator priv_find_with_hash
      ( const KeyType &key, KeyValueEqual equal_func, size_type &bucket_number, const std::size_t h, siterator &previt) const
   {
      bucket_number = this->priv_hash_to_bucket(h);
      bucket_type &b = this->priv_bucket_pointer()[bucket_number];
      previt = b.before_begin();
      if(constant_time_size && this->empty()){
         return this->priv_invalid_local_it();
      }

      siterator it = previt;
      ++it;

      while(it != b.end()){
         const value_type &v = this->priv_value_from_slist_node(it.pointed_node());
         if(compare_hash){
            std::size_t vh = this->priv_stored_or_compute_hash(v, store_hash_t());
            if(h == vh && equal_func(key, v)){
               return it;
            }
         }
         else if(equal_func(key, v)){
            return it;
         }
         if(optimize_multikey){
            previt = bucket_type::s_iterator_to
               (*group_functions_t::get_last_in_group
                  (detail::dcast_bucket_ptr<node>(it.pointed_node()), optimize_multikey_t()));
            it = previt;
         }
         else{
            previt = it;
         }
         ++it;
      }
      previt = b.before_begin();
      return this->priv_invalid_local_it();
   }

   iterator priv_insert_equal_with_hash(reference value, std::size_t hash_value)
   {
      size_type bucket_num;
      siterator prev;
      siterator it = this->priv_find_with_hash
         (value, this->priv_equal(), bucket_num, hash_value, prev);
      return this->priv_insert_equal_find(value, bucket_num, hash_value, it);
   }

   iterator priv_insert_equal_find(reference value, size_type bucket_num, std::size_t hash_value, siterator it)
   {
      bucket_type &b = this->priv_bucket_pointer()[bucket_num];
      bool found_equal = it != this->priv_invalid_local_it();
      if(!found_equal){
         it = b.before_begin();
      }
      //Now store hash if needed
      node_ptr n = pointer_traits<node_ptr>::pointer_to(this->priv_value_to_node(value));
      node_functions_t::store_hash(n, hash_value, store_hash_t());
      //Checks for some modes
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(n));
      //Shortcut for optimize_multikey cases
      if(optimize_multikey){
         node_ptr first_in_group = found_equal ?
            detail::dcast_bucket_ptr<node>(it.pointed_node()) : node_ptr();
         group_functions_t::insert_in_group(first_in_group, n, optimize_multikey_t());
      }
      //Update cache and increment size if needed
      this->priv_insertion_update_cache(bucket_num);
      this->priv_size_traits().increment();
      //Insert the element in the bucket after it
      return iterator(b.insert_after(it, *n), &this->get_bucket_value_traits());
   }

   template<class KeyType, class KeyHasher, class KeyValueEqual>
   std::pair<siterator, siterator> priv_equal_range
      ( const KeyType &key
      , KeyHasher hash_func
      , KeyValueEqual equal_func
      , size_type &bucket_number_first
      , size_type &bucket_number_second
      , size_type &cnt) const
   {
      std::size_t h;
      cnt = 0;
      siterator prev;
      //Let's see if the element is present
      std::pair<siterator, siterator> to_return
         ( this->priv_find(key, hash_func, equal_func, bucket_number_first, h, prev)
         , this->priv_invalid_local_it());
      if(to_return.first == to_return.second){
         bucket_number_second = bucket_number_first;
         return to_return;
      }
      {
         //If it's present, find the first that it's not equal in
         //the same bucket
         bucket_type &b = this->priv_bucket_pointer()[bucket_number_first];
         siterator it = to_return.first;
         if(optimize_multikey){
            to_return.second = bucket_type::s_iterator_to
               (*node_traits::get_next(group_functions_t::get_last_in_group
                  (detail::dcast_bucket_ptr<node>(it.pointed_node()), optimize_multikey_t())));
            cnt = std::distance(it, to_return.second);
            if(to_return.second !=  b.end()){
               bucket_number_second = bucket_number_first;
               return to_return;
            }
         }
         else{
            ++cnt;
            ++it;
            while(it != b.end()){
               const value_type &v = this->priv_value_from_slist_node(it.pointed_node());
               if(compare_hash){
                  std::size_t hv = this->priv_stored_or_compute_hash(v, store_hash_t());
                  if(hv != h || !equal_func(key, v)){
                     to_return.second = it;
                     bucket_number_second = bucket_number_first;
                     return to_return;
                  }
               }
               else if(!equal_func(key, v)){
                  to_return.second = it;
                  bucket_number_second = bucket_number_first;
                  return to_return;
               }
               ++it;
               ++cnt;
            }
         }
      }

      //If we reached the end, find the first, non-empty bucket
      for(bucket_number_second = bucket_number_first+1
         ; bucket_number_second != this->priv_bucket_count()
         ; ++bucket_number_second){
         bucket_type &b = this->priv_bucket_pointer()[bucket_number_second];
         if(!b.empty()){
            to_return.second = b.begin();
            return to_return;
         }
      }

      //Otherwise, return the end node
      to_return.second = this->priv_invalid_local_it();
      return to_return;
   }
   /// @endcond
};

/// @cond
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template < class T
         , bool UniqueKeys
         , class PackedOptions
         >
#else
template <class T, bool UniqueKeys, class ...Options>
#endif
struct make_bucket_traits
{
   //Real value traits must be calculated from options
   typedef typename detail::get_value_traits
      <T, typename PackedOptions::proto_value_traits>::type   value_traits;

   typedef typename PackedOptions::bucket_traits            specified_bucket_traits;

   //Real bucket traits must be calculated from options and calculated value_traits
   typedef typename detail::get_slist_impl
      <typename detail::reduced_slist_node_traits
         <typename value_traits::node_traits>::type
      >::type                                            slist_impl;

   typedef typename
      detail::if_c< detail::is_same
                     < specified_bucket_traits
                     , default_bucket_traits
                     >::value
                  , detail::bucket_traits_impl<slist_impl>
                  , specified_bucket_traits
                  >::type                                type;
};
/// @endcond

//! Helper metafunction to define a \c hashtable that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void, class O8 = void
                , class O9 = void, class O10= void
                >
#endif
struct make_hashtable
{
   /// @cond
   typedef typename pack_options
      < hashtable_defaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;

   typedef typename make_bucket_traits
            <T, false, packed_options>::type bucket_traits;

   typedef hashtable_impl
      < value_traits
      , typename packed_options::hash
      , typename packed_options::equal
      , typename packed_options::size_type
      , bucket_traits
      ,  (std::size_t(false)*hash_bool_flags::unique_keys_pos)
      |  (std::size_t(packed_options::constant_time_size)*hash_bool_flags::constant_time_size_pos)
      |  (std::size_t(packed_options::power_2_buckets)*hash_bool_flags::power_2_buckets_pos)
      |  (std::size_t(packed_options::cache_begin)*hash_bool_flags::cache_begin_pos)
      |  (std::size_t(packed_options::compare_hash)*hash_bool_flags::compare_hash_pos)
      |  (std::size_t(packed_options::incremental)*hash_bool_flags::incremental_pos)
      > implementation_defined;

   /// @endcond
   typedef implementation_defined type;
};

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

#if defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10>
#endif
class hashtable
   :  public make_hashtable<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
         >::type
{
   typedef typename make_hashtable<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(hashtable)

   public:
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;
   typedef typename Base::bucket_ptr         bucket_ptr;
   typedef typename Base::size_type          size_type;
   typedef typename Base::hasher             hasher;
   typedef typename Base::bucket_traits      bucket_traits;
   typedef typename Base::key_equal          key_equal;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename value_traits::value_type, T>::value));

   explicit hashtable ( const bucket_traits &b_traits
             , const hasher & hash_func = hasher()
             , const key_equal &equal_func = key_equal()
             , const value_traits &v_traits = value_traits())
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   hashtable(BOOST_RV_REF(hashtable) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   hashtable& operator=(BOOST_RV_REF(hashtable) x)
   {  return static_cast<hashtable&>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_HASHTABLE_HPP
