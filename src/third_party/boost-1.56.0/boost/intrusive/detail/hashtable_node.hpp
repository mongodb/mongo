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
#include <boost/intrusive/slist.hpp> //make_slist
#include <boost/intrusive/trivial_value_traits.hpp>
#include <cstddef>
#include <climits>
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

//We only support LLP64(Win64) or LP64(most Unix) data models
#ifdef _WIN64  //In 64 bit windows sizeof(size_t) == sizeof(unsigned long long)
   #define BOOST_INTRUSIVE_PRIME_C(NUMBER) NUMBER##ULL
   #define BOOST_INTRUSIVE_64_BIT_SIZE_T 1
#else //In 32 bit windows and 32/64 bit unixes sizeof(size_t) == sizeof(unsigned long)
   #define BOOST_INTRUSIVE_PRIME_C(NUMBER) NUMBER##UL
   #define BOOST_INTRUSIVE_64_BIT_SIZE_T (((((ULONG_MAX>>16)>>16)>>16)>>15) != 0)
#endif

template<int Dummy>
const std::size_t prime_list_holder<Dummy>::prime_list[] = {
   BOOST_INTRUSIVE_PRIME_C(3),                     BOOST_INTRUSIVE_PRIME_C(7),
   BOOST_INTRUSIVE_PRIME_C(11),                    BOOST_INTRUSIVE_PRIME_C(17),
   BOOST_INTRUSIVE_PRIME_C(29),                    BOOST_INTRUSIVE_PRIME_C(53),
   BOOST_INTRUSIVE_PRIME_C(97),                    BOOST_INTRUSIVE_PRIME_C(193),
   BOOST_INTRUSIVE_PRIME_C(389),                   BOOST_INTRUSIVE_PRIME_C(769),
   BOOST_INTRUSIVE_PRIME_C(1543),                  BOOST_INTRUSIVE_PRIME_C(3079),
   BOOST_INTRUSIVE_PRIME_C(6151),                  BOOST_INTRUSIVE_PRIME_C(12289),
   BOOST_INTRUSIVE_PRIME_C(24593),                 BOOST_INTRUSIVE_PRIME_C(49157),
   BOOST_INTRUSIVE_PRIME_C(98317),                 BOOST_INTRUSIVE_PRIME_C(196613),
   BOOST_INTRUSIVE_PRIME_C(393241),                BOOST_INTRUSIVE_PRIME_C(786433),
   BOOST_INTRUSIVE_PRIME_C(1572869),               BOOST_INTRUSIVE_PRIME_C(3145739),
   BOOST_INTRUSIVE_PRIME_C(6291469),               BOOST_INTRUSIVE_PRIME_C(12582917),
   BOOST_INTRUSIVE_PRIME_C(25165843),              BOOST_INTRUSIVE_PRIME_C(50331653),
   BOOST_INTRUSIVE_PRIME_C(100663319),             BOOST_INTRUSIVE_PRIME_C(201326611),
   BOOST_INTRUSIVE_PRIME_C(402653189),             BOOST_INTRUSIVE_PRIME_C(805306457),
   BOOST_INTRUSIVE_PRIME_C(1610612741),            BOOST_INTRUSIVE_PRIME_C(3221225473),
#if BOOST_INTRUSIVE_64_BIT_SIZE_T
   //Taken from Boost.MultiIndex code, thanks to Joaquin M Lopez Munoz.
   BOOST_INTRUSIVE_PRIME_C(6442450939),            BOOST_INTRUSIVE_PRIME_C(12884901893),
   BOOST_INTRUSIVE_PRIME_C(25769803751),           BOOST_INTRUSIVE_PRIME_C(51539607551),
   BOOST_INTRUSIVE_PRIME_C(103079215111),          BOOST_INTRUSIVE_PRIME_C(206158430209),
   BOOST_INTRUSIVE_PRIME_C(412316860441),          BOOST_INTRUSIVE_PRIME_C(824633720831),
   BOOST_INTRUSIVE_PRIME_C(1649267441651),         BOOST_INTRUSIVE_PRIME_C(3298534883309),
   BOOST_INTRUSIVE_PRIME_C(6597069766657),         BOOST_INTRUSIVE_PRIME_C(13194139533299),
   BOOST_INTRUSIVE_PRIME_C(26388279066623),        BOOST_INTRUSIVE_PRIME_C(52776558133303),
   BOOST_INTRUSIVE_PRIME_C(105553116266489),       BOOST_INTRUSIVE_PRIME_C(211106232532969),
   BOOST_INTRUSIVE_PRIME_C(422212465066001),       BOOST_INTRUSIVE_PRIME_C(844424930131963),
   BOOST_INTRUSIVE_PRIME_C(1688849860263953),      BOOST_INTRUSIVE_PRIME_C(3377699720527861),
   BOOST_INTRUSIVE_PRIME_C(6755399441055731),      BOOST_INTRUSIVE_PRIME_C(13510798882111483),
   BOOST_INTRUSIVE_PRIME_C(27021597764222939),     BOOST_INTRUSIVE_PRIME_C(54043195528445957),
   BOOST_INTRUSIVE_PRIME_C(108086391056891903),    BOOST_INTRUSIVE_PRIME_C(216172782113783843),
   BOOST_INTRUSIVE_PRIME_C(432345564227567621),    BOOST_INTRUSIVE_PRIME_C(864691128455135207),
   BOOST_INTRUSIVE_PRIME_C(1729382256910270481),   BOOST_INTRUSIVE_PRIME_C(3458764513820540933),
   BOOST_INTRUSIVE_PRIME_C(6917529027641081903),   BOOST_INTRUSIVE_PRIME_C(13835058055282163729),
   BOOST_INTRUSIVE_PRIME_C(18446744073709551557)
#else
   BOOST_INTRUSIVE_PRIME_C(4294967291)
#endif
   };

#undef BOOST_INTRUSIVE_PRIME_C
#undef BOOST_INTRUSIVE_64_BIT_SIZE_T

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
	   , boost::intrusive::size_type<std::size_t>
      >::type
   {};
};

}  //namespace detail {

template<class BucketValueTraits, bool IsConst>
class hashtable_iterator
{
   typedef std::iterator
         < std::forward_iterator_tag
         , typename BucketValueTraits::value_traits::value_type
         , typename pointer_traits<typename BucketValueTraits::value_traits::value_type*>::difference_type
         , typename detail::add_const_if_c
                     <typename BucketValueTraits::value_traits::value_type, IsConst>::type *
         , typename detail::add_const_if_c
                     <typename BucketValueTraits::value_traits::value_type, IsConst>::type &
         >  iterator_traits;

   typedef typename BucketValueTraits::value_traits          value_traits;
   typedef typename BucketValueTraits::bucket_traits         bucket_traits;
   typedef typename value_traits::node_traits                node_traits;
   typedef typename detail::get_slist_impl
      <typename detail::reduced_slist_node_traits
         <typename value_traits::node_traits>::type
      >::type                                                     slist_impl;
   typedef typename slist_impl::iterator                          siterator;
   typedef typename slist_impl::const_iterator                    const_siterator;
   typedef detail::bucket_impl<slist_impl>                        bucket_type;

   typedef typename pointer_traits
      <typename value_traits::pointer>::template rebind_pointer
         < const BucketValueTraits >::type                        const_bucketvaltraits_ptr;
   typedef typename slist_impl::size_type                         size_type;


   static typename node_traits::node_ptr downcast_bucket(typename bucket_type::node_ptr p)
   {
      return pointer_traits<typename node_traits::node_ptr>::
         pointer_to(static_cast<typename node_traits::node&>(*p));
   }

   public:
   typedef typename iterator_traits::difference_type    difference_type;
   typedef typename iterator_traits::value_type         value_type;
   typedef typename iterator_traits::pointer            pointer;
   typedef typename iterator_traits::reference          reference;
   typedef typename iterator_traits::iterator_category  iterator_category;

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
      return boost::intrusive::detail::to_raw_pointer(this->priv_value_traits().to_value_ptr
         (downcast_bucket(slist_it_.pointed_node())));
   }

   const const_bucketvaltraits_ptr &get_bucket_value_traits() const
   {  return traitsptr_;  }

   const value_traits &priv_value_traits() const
   {  return traitsptr_->priv_value_traits();  }

   const bucket_traits &priv_bucket_traits() const
   {  return traitsptr_->priv_bucket_traits();  }

   private:
   void increment()
   {
      const bucket_traits &rbuck_traits = this->priv_bucket_traits();
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
