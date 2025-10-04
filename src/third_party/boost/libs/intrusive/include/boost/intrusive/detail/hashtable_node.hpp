/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2014
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

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/trivial_value_traits.hpp>
#include <boost/intrusive/detail/common_slist_algorithms.hpp>
#include <boost/intrusive/detail/iiterator.hpp>
#include <boost/intrusive/detail/slist_iterator.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>
#include <cstddef>
#include <climits>
#include <boost/move/core.hpp>


namespace boost {
namespace intrusive {

template <class NodeTraits>
struct bucket_impl
   : public NodeTraits::node
{
   public:
   typedef NodeTraits node_traits;

   private:
   typedef typename node_traits::node_ptr          node_ptr;
   typedef typename node_traits::const_node_ptr    const_node_ptr;

   typedef detail::common_slist_algorithms<NodeTraits> algo_t;

   public:
   inline bucket_impl()
   {}

   inline bucket_impl(const bucket_impl &)
   {}

   inline ~bucket_impl()
   {}

   inline bucket_impl &operator=(const bucket_impl&)
   {  return *this;  }

   inline node_ptr get_node_ptr()
   {  return pointer_traits<node_ptr>::pointer_to(*this);  }

   inline const_node_ptr get_node_ptr() const
   {  return pointer_traits<const_node_ptr>::pointer_to(*this);  }

   inline node_ptr begin_ptr()
   {  return node_traits::get_next(get_node_ptr());  }
};



template <class NodeTraits>
struct hash_reduced_slist_node_traits
{
   template <class U> static detail::no_type test(...);
   template <class U> static detail::yes_type test(typename U::reduced_slist_node_traits*);
   static const bool value = sizeof(test<NodeTraits>(0)) == sizeof(detail::yes_type);
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

template<class BucketValueTraits, bool LinearBuckets, bool IsConst>
class hashtable_iterator
{
   typedef typename BucketValueTraits::value_traits            value_traits;
   typedef typename BucketValueTraits::bucket_traits           bucket_traits;

   typedef iiterator< value_traits, IsConst
                    , std::forward_iterator_tag>   types_t;
   public:
   typedef typename types_t::iterator_type::difference_type    difference_type;
   typedef typename types_t::iterator_type::value_type         value_type;
   typedef typename types_t::iterator_type::pointer            pointer;
   typedef typename types_t::iterator_type::reference          reference;
   typedef typename types_t::iterator_type::iterator_category  iterator_category;

   private:
   typedef typename value_traits::node_traits                  node_traits;
   typedef typename node_traits::node_ptr                      node_ptr;
   typedef typename BucketValueTraits::bucket_type             bucket_type;
   typedef typename bucket_type::node_traits                   slist_node_traits;
   typedef typename slist_node_traits::node_ptr                slist_node_ptr;
   typedef trivial_value_traits
      <slist_node_traits, normal_link>                         slist_value_traits;
   typedef slist_iterator<slist_value_traits, false>           siterator;
   typedef slist_iterator<slist_value_traits, true>            const_siterator;
   typedef circular_slist_algorithms<slist_node_traits>        slist_node_algorithms;

   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < const BucketValueTraits >::type                     const_bucketvaltraits_ptr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , hashtable_iterator<BucketValueTraits, LinearBuckets, false>
                  , nat>::type                                 nonconst_iterator;

   inline static node_ptr downcast_bucket(typename bucket_type::node_traits::node_ptr p)
   {
      return pointer_traits<node_ptr>::
         pointer_to(static_cast<typename node_traits::node&>(*p));
   }

   public:

   inline hashtable_iterator ()
      : slist_it_()  //Value initialization to achieve "null iterators" (N3644)
   {}

   inline explicit hashtable_iterator(siterator ptr, const BucketValueTraits *cont)
      : slist_it_ (ptr)
      , traitsptr_ (cont ? pointer_traits<const_bucketvaltraits_ptr>::pointer_to(*cont) : const_bucketvaltraits_ptr() )
   {}

   inline hashtable_iterator(const hashtable_iterator &other)
      :  slist_it_(other.slist_it()), traitsptr_(other.get_bucket_value_traits())
   {}

   inline hashtable_iterator(const nonconst_iterator &other)
      :  slist_it_(other.slist_it()), traitsptr_(other.get_bucket_value_traits())
   {}

   inline const siterator &slist_it() const
   { return slist_it_; }

   inline hashtable_iterator<BucketValueTraits, LinearBuckets, false> unconst() const
   {  return hashtable_iterator<BucketValueTraits, LinearBuckets, false>(this->slist_it(), this->get_bucket_value_traits());   }

   inline hashtable_iterator& operator++()
   {  this->increment();   return *this;   }

   inline hashtable_iterator &operator=(const hashtable_iterator &other)
   {  slist_it_ = other.slist_it(); traitsptr_ = other.get_bucket_value_traits();   return *this;  }

   inline hashtable_iterator operator++(int)
   {
      hashtable_iterator result (*this);
      this->increment();
      return result;
   }

   inline friend bool operator== (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return i.slist_it_ == i2.slist_it_; }

   inline friend bool operator!= (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return !(i == i2); }

   inline reference operator*() const
   { return *this->operator ->(); }

   inline pointer operator->() const
   {
      return this->priv_value_traits().to_value_ptr
         (downcast_bucket(slist_it_.pointed_node()));
   }

   inline const_bucketvaltraits_ptr get_bucket_value_traits() const
   {  return traitsptr_;  }

   inline const value_traits &priv_value_traits() const
   {  return traitsptr_->priv_value_traits();  }

   private:

   void increment()
   {
      bucket_type* const buckets = boost::movelib::to_raw_pointer(traitsptr_->priv_bucket_traits().bucket_begin());
      const std::size_t buckets_len = traitsptr_->priv_bucket_traits().bucket_count();

      ++slist_it_;
      const slist_node_ptr n = slist_it_.pointed_node();
      const siterator first_bucket_bbegin(buckets->get_node_ptr());
      if(first_bucket_bbegin.pointed_node() <= n && n <= buckets[buckets_len-1].get_node_ptr()){
         //If one-past the node is inside the bucket then look for the next non-empty bucket
         //1. get the bucket_impl from the iterator
         const bucket_type &b = static_cast<const bucket_type&>(*n);

         //2. Now just calculate the index b has in the bucket array
         std::size_t n_bucket = static_cast<std::size_t>(&b - buckets);

         //3. Iterate until a non-empty bucket is found
         slist_node_ptr bucket_nodeptr = buckets->get_node_ptr();
         do{
            if (++n_bucket >= buckets_len){  //bucket overflow, return end() iterator
               slist_it_ = first_bucket_bbegin;
               return;
            }
            bucket_nodeptr = buckets[n_bucket].get_node_ptr();
         }
         while (slist_node_algorithms::is_empty(bucket_nodeptr));
         slist_it_ = siterator(bucket_nodeptr);
         ++slist_it_;
      }
      else{
         //++slist_it_ yield to a valid object
      }
   }

   siterator                  slist_it_;
   const_bucketvaltraits_ptr  traitsptr_;
};

template<class BucketValueTraits, bool IsConst>
class hashtable_iterator<BucketValueTraits, true, IsConst>
{
   typedef typename BucketValueTraits::value_traits            value_traits;
   typedef typename BucketValueTraits::bucket_traits           bucket_traits;

   typedef iiterator< value_traits, IsConst
                    , std::forward_iterator_tag>   types_t;
   public:
   typedef typename types_t::iterator_type::difference_type    difference_type;
   typedef typename types_t::iterator_type::value_type         value_type;
   typedef typename types_t::iterator_type::pointer            pointer;
   typedef typename types_t::iterator_type::reference          reference;
   typedef typename types_t::iterator_type::iterator_category  iterator_category;

   private:
   typedef typename value_traits::node_traits                  node_traits;
   typedef typename node_traits::node_ptr                      node_ptr;
   typedef typename BucketValueTraits::bucket_type             bucket_type;
   typedef typename BucketValueTraits::bucket_ptr              bucket_ptr;
   typedef typename bucket_type::node_traits                   slist_node_traits;
   typedef linear_slist_algorithms<slist_node_traits>          slist_node_algorithms;
   typedef typename slist_node_traits::node_ptr                slist_node_ptr;
   typedef trivial_value_traits
      <slist_node_traits, normal_link>                         slist_value_traits;
   typedef slist_iterator<slist_value_traits, false>           siterator;
   typedef slist_iterator<slist_value_traits, true>            const_siterator;

   static const bool stateful_value_traits =
      detail::is_stateful_value_traits<value_traits>::value;

   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         < const value_traits >::type                          const_value_traits_ptr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , hashtable_iterator<BucketValueTraits, true, false>
                  , nat>::type                                 nonconst_iterator;

   inline static node_ptr downcast_bucket(slist_node_ptr p)
   {
      return pointer_traits<node_ptr>::
         pointer_to(static_cast<typename node_traits::node&>(*p));
   }

   public:

   inline hashtable_iterator ()
      : slist_it_()  //Value initialization to achieve "null iterators" (N3644)
      , members_()
   {}

   inline explicit hashtable_iterator(siterator ptr, bucket_ptr bp, const_value_traits_ptr traits_ptr)
      : slist_it_ (ptr)
      , members_ (bp, traits_ptr)
   {}

   inline hashtable_iterator(const hashtable_iterator &other)
      :  slist_it_(other.slist_it()), members_(other.get_bucket_ptr(), other.get_value_traits())
   {}

   inline hashtable_iterator(const nonconst_iterator &other)
      :  slist_it_(other.slist_it()), members_(other.get_bucket_ptr(), other.get_value_traits())
   {}

   inline const siterator &slist_it() const
   { return slist_it_; }

   inline hashtable_iterator<BucketValueTraits, true, false> unconst() const
   {  return hashtable_iterator<BucketValueTraits, true, false>(this->slist_it(), members_.nodeptr_, members_.get_ptr());   }

   inline hashtable_iterator& operator++()
   {  this->increment();   return *this;   }

   inline hashtable_iterator &operator=(const hashtable_iterator &other)
   {  slist_it_ = other.slist_it(); members_ = other.members_;  return *this;  }

   inline hashtable_iterator operator++(int)
   {
      hashtable_iterator result (*this);
      this->increment();
      return result;
   }

   inline friend bool operator== (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return i.slist_it_ == i2.slist_it_; }

   inline friend bool operator!= (const hashtable_iterator& i, const hashtable_iterator& i2)
   { return i.slist_it_ != i2.slist_it_; }

   inline reference operator*() const
   { return *this->operator ->(); }

   inline pointer operator->() const
   { return this->operator_arrow(detail::bool_<stateful_value_traits>()); }

   inline const_value_traits_ptr get_value_traits() const
   {  return members_.get_ptr(); }

   inline bucket_ptr get_bucket_ptr() const
   {  return members_.nodeptr_; }

   private:

   inline pointer operator_arrow(detail::false_) const
   { return value_traits::to_value_ptr(downcast_bucket(slist_it_.pointed_node())); }

   inline pointer operator_arrow(detail::true_) const
   { return this->get_value_traits()->to_value_ptr(downcast_bucket(slist_it_.pointed_node())); }

   void increment()
   {
      ++slist_it_;
      if (slist_it_ == siterator()){
         slist_node_ptr bucket_nodeptr;
         do {
            ++members_.nodeptr_;
             bucket_nodeptr = members_.nodeptr_->get_node_ptr();
         }while(slist_node_algorithms::is_empty(bucket_nodeptr));
         slist_it_ = siterator(slist_node_traits::get_next(bucket_nodeptr));
      }
   }

   siterator   slist_it_;
   iiterator_members<bucket_ptr, const_value_traits_ptr, stateful_value_traits> members_;
};

}  //namespace intrusive {
}  //namespace boost {

#endif
