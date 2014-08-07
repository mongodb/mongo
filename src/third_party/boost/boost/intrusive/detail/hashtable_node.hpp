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

#ifndef BOOST_INTRUSIVE_HASHTABLE_NODE_HPP
#define BOOST_INTRUSIVE_HASHTABLE_NODE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <iterator>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/circular_list_algorithms.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/utilities.hpp>
//#include <boost/intrusive/detail/slist_node.hpp> //remove-me
#include <boost/intrusive/pointer_traits.hpp>
#include <cstddef>
#include <boost/pointer_cast.hpp>
#include <boost/move/move.hpp>


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

template<class Container, bool IsConst>
class hashtable_iterator
   :  public std::iterator
         < std::forward_iterator_tag
         , typename Container::value_type
         , typename pointer_traits<typename Container::value_type*>::difference_type
         , typename detail::add_const_if_c
                     <typename Container::value_type, IsConst>::type *
         , typename detail::add_const_if_c
                     <typename Container::value_type, IsConst>::type &
         >
{
   typedef typename Container::real_value_traits                  real_value_traits;
   typedef typename Container::siterator                          siterator;
   typedef typename Container::const_siterator                    const_siterator;
   typedef typename Container::bucket_type                        bucket_type;

   typedef typename pointer_traits
      <typename Container::pointer>::template rebind_pointer
         < const Container >::type                                const_cont_ptr;
   typedef typename Container::size_type                          size_type;

   static typename Container::node_ptr downcast_bucket(typename bucket_type::node_ptr p)
   {
      return pointer_traits<typename Container::node_ptr>::
         pointer_to(static_cast<typename Container::node&>(*p));
   }

   public:
   typedef typename Container::value_type    value_type;
   typedef  typename detail::add_const_if_c
                     <typename Container::value_type, IsConst>::type *pointer;
   typedef typename detail::add_const_if_c
                     <typename Container::value_type, IsConst>::type &reference;

   hashtable_iterator ()
   {}

   explicit hashtable_iterator(siterator ptr, const Container *cont)
      :  slist_it_ (ptr),   cont_ (cont ? pointer_traits<const_cont_ptr>::pointer_to(*cont) : const_cont_ptr() )
   {}

   hashtable_iterator(const hashtable_iterator<Container, false> &other)
      :  slist_it_(other.slist_it()), cont_(other.get_container())
   {}

   const siterator &slist_it() const
   { return slist_it_; }

   hashtable_iterator<Container, false> unconst() const
   {  return hashtable_iterator<Container, false>(this->slist_it(), this->get_container());   }

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
      return boost::intrusive::detail::to_raw_pointer(this->get_real_value_traits()->to_value_ptr
         (downcast_bucket(slist_it_.pointed_node())));
   }

   const const_cont_ptr &get_container() const
   {  return cont_;  }

   const real_value_traits *get_real_value_traits() const
   {  return &this->get_container()->get_real_value_traits();  }

   private:
   void increment()
   {
      const Container *cont =  boost::intrusive::detail::to_raw_pointer(cont_);
      bucket_type* buckets = boost::intrusive::detail::to_raw_pointer(cont->bucket_pointer());
      size_type   buckets_len    = cont->bucket_count();

      ++slist_it_;
      if(buckets[0].cend().pointed_node()    <= slist_it_.pointed_node() && 
         slist_it_.pointed_node()<= buckets[buckets_len].cend().pointed_node()      ){
         //Now get the bucket_impl from the iterator
         const bucket_type &b = static_cast<const bucket_type&>
            (bucket_type::slist_type::container_from_end_iterator(slist_it_));

         //Now just calculate the index b has in the bucket array
         size_type n_bucket = static_cast<size_type>(&b - &buckets[0]);
         do{
            if (++n_bucket == buckets_len){
               slist_it_ = (&buckets[0] + buckets_len)->end();
               break;
            }
            slist_it_ = buckets[n_bucket].begin();
         }
         while (slist_it_ == buckets[n_bucket].end());
      }
   }

   siterator      slist_it_;
   const_cont_ptr cont_;
};

}  //namespace detail {
}  //namespace intrusive {
}  //namespace boost {

#endif
