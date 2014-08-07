/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_HASHTABLE_NODE_HPP
#define BOOST_INTRUSIVE_HASHTABLE_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/circular_list_algorithms.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/slist.hpp> //remove-me
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/trivial_value_traits.hpp>
#include <cstddef>
#include <boost/type_traits/make_unsigned.hpp>
#include <boost/pointer_cast.hpp>
#include <boost/move/core.hpp>


namespace boost {
namespace intrusive {
namespace detail {

template<int Dummy = 0>
struct prime_list_holder
{
   static const std::size_t prime_list[];
   static const std::size_t prime_list_size;
};

template<int Dummy>
const std::size_t prime_list_holder<Dummy>::prime_list[] = {
   3ul, 7ul, 11ul, 17ul, 29ul,
   53ul, 97ul, 193ul, 389ul, 769ul,
   1543ul, 3079ul, 6151ul, 12289ul, 24593ul,
   49157ul, 98317ul, 196613ul, 393241ul, 786433ul,
   1572869ul, 3145739ul, 6291469ul, 12582917ul, 25165843ul,
   50331653ul, 100663319ul, 201326611ul, 402653189ul, 805306457ul,
   1610612741ul, 3221225473ul, 4294967291ul };

template<int Dummy>
const std::size_t prime_list_holder<Dummy>::prime_list_size
   = sizeof(prime_list)/sizeof(std::size_t);

template <class Slist>
struct bucket_impl : public Slist
{
   typedef Slist slist_type;
   bucket_impl()
   {}

   bucket_impl(const bucket_impl &)
   {}

   ~bucket_impl()
   {
      //This bucket is still being used!
      BOOST_INTRUSIVE_INVARIANT_ASSERT(Slist::empty());
   }

   bucket_impl &operator=(const bucket_impl&)
   {
      //This bucket is still in use!
      BOOST_INTRUSIVE_INVARIANT_ASSERT(Slist::empty());
      //Slist::clear();
      return *this;
   }
};

template<class Slist>
struct bucket_traits_impl
{
   private:
   BOOST_COPYABLE_AND_MOVABLE(bucket_traits_impl)

   public:
   /// @cond

   typedef typename pointer_traits
      <typename Slist::pointer>::template rebind_pointer
         < bucket_impl<Slist> >::type                                bucket_ptr;
   typedef Slist slist;
   typedef typename Slist::size_type size_type;
   /// @endcond

   bucket_traits_impl(bucket_ptr buckets, size_type len)
      :  buckets_(buckets), buckets_len_(len)
   {}

   bucket_traits_impl(const bucket_traits_impl &x)
      : buckets_(x.buckets_), buckets_len_(x.buckets_len_)
   {}

   bucket_traits_impl(BOOST_RV_REF(bucket_traits_impl) x)
      : buckets_(x.buckets_), buckets_len_(x.buckets_len_)
   {  x.buckets_ = bucket_ptr();   x.buckets_len_ = 0;  }

   bucket_traits_impl& operator=(BOOST_RV_REF(bucket_traits_impl) x)
   {
      buckets_ = x.buckets_; buckets_len_ = x.buckets_len_;
      x.buckets_ = bucket_ptr();   x.buckets_len_ = 0; return *this;
   }

   bucket_traits_impl& operator=(BOOST_COPY_ASSIGN_REF(bucket_traits_impl) x)
   {
      buckets_ = x.buckets_;  buckets_len_ = x.buckets_len_; return *this;
   }

   const bucket_ptr &bucket_begin() const
   {  return buckets_;  }

   size_type  bucket_count() const
   {  return buckets_len_;  }

   private:
   bucket_ptr  buckets_;
   size_type   buckets_len_;
};

template <class NodeTraits>
struct hash_reduced_slist_node_traits
{
   template <class U> static detail::one test(...);
   template <class U> static detail::two test(typename U::reduced_slist_node_traits* = 0);
   static const bool value = sizeof(test<NodeTraits>(0)) == sizeof(detail::two);
};

template <class NodeTraits>
struct apply_reduced_slist_node_traits
{
   typedef typename NodeTraits::reduced_slist_node_traits type;
};

template <class NodeTraits>
struct reduced_slist_node_traits
{
   typedef typename detail::eval_if_c
      < hash_reduced_slist_node_traits<NodeTraits>::value
      , apply_reduced_slist_node_traits<NodeTraits>
      , detail::identity<NodeTraits>
      >::type type;
};

template<class NodeTraits>
struct get_slist_impl
{
   typedef trivial_value_traits<NodeTraits, normal_link> trivial_traits;

   //Reducing symbol length
   struct type : make_slist
      < typename NodeTraits::node
      , boost::intrusive::value_traits<trivial_traits>
      , boost::intrusive::constant_time_size<false>
	  , boost::intrusive::size_type<typename boost::make_unsigned
         <typename pointer_traits<typename NodeTraits::node_ptr>::difference_type>::type >
      >::type
   {};
};

}  //namespace detail {

template<class BucketValueTraits, bool IsConst>
class hashtable_iterator
   :  public std::iterator
         < std::forward_iterator_tag
         , typename BucketValueTraits::real_value_traits::value_type
         , typename pointer_traits<typename BucketValueTraits::real_value_traits::value_type*>::difference_type
         , typename detail::add_const_if_c
                     <typename BucketValueTraits::real_value_traits::value_type, IsConst>::type *
         , typename detail::add_const_if_c
                     <typename BucketValueTraits::real_value_traits::value_type, IsConst>::type &
         >
{
   typedef typename BucketValueTraits::real_value_traits          real_value_traits;
   typedef typename BucketValueTraits::real_bucket_traits         real_bucket_traits;
   typedef typename real_value_traits::node_traits                node_traits;
   typedef typename detail::get_slist_impl
      <typename detail::reduced_slist_node_traits
         <typename real_value_traits::node_traits>::type
      >::type                                                     slist_impl;
   typedef typename slist_impl::iterator                          siterator;
   typedef typename slist_impl::const_iterator                    const_siterator;
   typedef detail::bucket_impl<slist_impl>                        bucket_type;

   typedef typename pointer_traits
      <typename real_value_traits::pointer>::template rebind_pointer
         < const BucketValueTraits >::type                        const_bucketvaltraits_ptr;
   typedef typename slist_impl::size_type                         size_type;


   static typename node_traits::node_ptr downcast_bucket(typename bucket_type::node_ptr p)
   {
      return pointer_traits<typename node_traits::node_ptr>::
         pointer_to(static_cast<typename node_traits::node&>(*p));
   }

   public:
   typedef typename real_value_traits::value_type    value_type;
   typedef typename detail::add_const_if_c<value_type, IsConst>::type *pointer;
   typedef typename detail::add_const_if_c<value_type, IsConst>::type &reference;

   hashtable_iterator ()
   {}

   explicit hashtable_iterator(siterator ptr, const BucketValueTraits *cont)
      :  slist_it_ (ptr),   traitsptr_ (cont ? pointer_traits<const_bucketvaltraits_ptr>::pointer_to(*cont) : const_bucketvaltraits_ptr() )
   {}

   hashtable_iterator(const hashtable_iterator<BucketValueTraits, false> &other)
      :  slist_it_(other.slist_it()), traitsptr_(other.get_bucket_value_traits())
   {}

   const siterator &slist_it() const
   { return slist_it_; }

   hashtable_iterator<BucketValueTraits, false> unconst() const
   {  return hashtable_iterator<BucketValueTraits, false>(this->slist_it(), this->get_bucket_value_traits());   }

   public:
   hashtable_iterator& operator++()
   {  this->increment();   return *this;   }

   hashtable_iterator operator++(int)
   {
      hashtable_iterator result (*this);
      this->increment();
      return result;
   }

   friend bool operator== (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return i.slist_it_ == i2.slist_it_; }

   friend bool operator!= (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return !(i == i2); }

   reference operator*() const
   { return *this->operator ->(); }

   pointer operator->() const
   {
      return boost::intrusive::detail::to_raw_pointer(this->priv_real_value_traits().to_value_ptr
         (downcast_bucket(slist_it_.pointed_node())));
   }

   const const_bucketvaltraits_ptr &get_bucket_value_traits() const
   {  return traitsptr_;  }

   const real_value_traits &priv_real_value_traits() const
   {  return traitsptr_->priv_real_value_traits();  }

   const real_bucket_traits &priv_real_bucket_traits() const
   {  return traitsptr_->priv_real_bucket_traits();  }

   private:
   void increment()
   {
      const real_bucket_traits &rbuck_traits = this->priv_real_bucket_traits();
      bucket_type* const buckets = boost::intrusive::detail::to_raw_pointer(rbuck_traits.bucket_begin());
      const size_type buckets_len = rbuck_traits.bucket_count();

      ++slist_it_;
      const typename slist_impl::node_ptr n = slist_it_.pointed_node();
      const siterator first_bucket_bbegin = buckets->end();
      if(first_bucket_bbegin.pointed_node() <= n && n <= buckets[buckets_len-1].cend().pointed_node()){
         //If one-past the node is inside the bucket then look for the next non-empty bucket
         //1. get the bucket_impl from the iterator
         const bucket_type &b = static_cast<const bucket_type&>
            (bucket_type::slist_type::container_from_end_iterator(slist_it_));

         //2. Now just calculate the index b has in the bucket array
         size_type n_bucket = static_cast<size_type>(&b - buckets);

         //3. Iterate until a non-empty bucket is found
         do{
            if (++n_bucket >= buckets_len){  //bucket overflow, return end() iterator
               slist_it_ = buckets->before_begin();
               return;
            }
         }
         while (buckets[n_bucket].empty());
         slist_it_ = buckets[n_bucket].begin();
      }
      else{
         //++slist_it_ yield to a valid object
      }
   }

   siterator                  slist_it_;
   const_bucketvaltraits_ptr  traitsptr_;
};

}  //namespace intrusive {
}  //namespace boost {

#endif
