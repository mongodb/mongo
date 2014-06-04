/* Copyright 2003-2008 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_BUCKET_ARRAY_HPP
#define BOOST_MULTI_INDEX_DETAIL_BUCKET_ARRAY_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <algorithm>
#include <boost/multi_index/detail/auto_space.hpp>
#include <boost/multi_index/detail/hash_index_node.hpp>
#include <boost/multi_index/detail/prevent_eti.hpp>
#include <boost/noncopyable.hpp>
#include <cstddef>
#include <limits.h>

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
#include <boost/archive/archive_exception.hpp>
#include <boost/serialization/access.hpp>
#include <boost/throw_exception.hpp> 
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* bucket structure for use by hashed indices */

class bucket_array_base:private noncopyable
{
protected:
  inline static std::size_t next_prime(std::size_t n)
  {
    static const std::size_t prime_list[]={
      53ul, 97ul, 193ul, 389ul, 769ul,
      1543ul, 3079ul, 6151ul, 12289ul, 24593ul,
      49157ul, 98317ul, 196613ul, 393241ul, 786433ul,
      1572869ul, 3145739ul, 6291469ul, 12582917ul, 25165843ul,
      50331653ul, 100663319ul, 201326611ul, 402653189ul, 805306457ul,
      1610612741ul, 3221225473ul,

#if ((((ULONG_MAX>>16)>>16)>>16)>>15)==0 /* unsigned long less than 64 bits */
      4294967291ul
#else
      /* obtained with aid from
       *   http://javaboutique.internet.com/prime_numb/
       *   http://www.rsok.com/~jrm/next_ten_primes.html
       * and verified with
       *   http://www.alpertron.com.ar/ECM.HTM
       */

      6442450939ul, 12884901893ul, 25769803751ul, 51539607551ul,
      103079215111ul, 206158430209ul, 412316860441ul, 824633720831ul,
      1649267441651ul, 3298534883309ul, 6597069766657ul, 13194139533299ul,
      26388279066623ul, 52776558133303ul, 105553116266489ul, 211106232532969ul,
      422212465066001ul, 844424930131963ul, 1688849860263953ul,
      3377699720527861ul, 6755399441055731ul, 13510798882111483ul,
      27021597764222939ul, 54043195528445957ul, 108086391056891903ul,
      216172782113783843ul, 432345564227567621ul, 864691128455135207ul,
      1729382256910270481ul, 3458764513820540933ul, 6917529027641081903ul,
      13835058055282163729ul, 18446744073709551557ul
#endif

    };
    static const std::size_t prime_list_size=
      sizeof(prime_list)/sizeof(prime_list[0]);

    std::size_t const *bound=
      std::lower_bound(prime_list,prime_list+prime_list_size,n);
    if(bound==prime_list+prime_list_size)bound--;
    return *bound;
  }
};

template<typename Allocator>
class bucket_array:public bucket_array_base
{
  typedef typename prevent_eti<
    Allocator,
    hashed_index_node_impl<
      typename boost::detail::allocator::rebind_to<
        Allocator,
        char
      >::type
    >
  >::type                                           node_impl_type;

public:
  typedef typename node_impl_type::pointer          pointer;

  bucket_array(const Allocator& al,pointer end_,std::size_t size):
    size_(bucket_array_base::next_prime(size)),
    spc(al,size_+1)
  {
    clear();
    end()->next()=end_;
    end_->next()=end();
  }

  std::size_t size()const
  {
    return size_;
  }

  std::size_t position(std::size_t hash)const
  {
    return hash%size_;
  }

  pointer begin()const{return buckets();}
  pointer end()const{return buckets()+size_;}
  pointer at(std::size_t n)const{return buckets()+n;}

  std::size_t first_nonempty(std::size_t n)const
  {
    for(;;++n){
      pointer x=at(n);
      if(x->next()!=x)return n;
    }
  }

  void clear()
  {
    for(pointer x=begin(),y=end();x!=y;++x)x->next()=x;
  }

  void swap(bucket_array& x)
  {
    std::swap(size_,x.size_);
    spc.swap(x.spc);
  }

private:
  std::size_t                          size_;
  auto_space<node_impl_type,Allocator> spc;

  pointer buckets()const
  {
    return spc.data();
  }

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
  friend class boost::serialization::access;
  
  /* bucket_arrays do not emit any kind of serialization info. They are
   * fed to Boost.Serialization as hashed index iterators need to track
   * them during serialization.
   */

  template<class Archive>
  void serialize(Archive&,const unsigned int)
  {
  }
#endif
};

template<typename Allocator>
void swap(bucket_array<Allocator>& x,bucket_array<Allocator>& y)
{
  x.swap(y);
}

} /* namespace multi_index::detail */

} /* namespace multi_index */

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
/* bucket_arrays never get constructed directly by Boost.Serialization,
 * as archives are always fed pointers to previously existent
 * arrays. So, if this is called it means we are dealing with a
 * somehow invalid archive.
 */

#if defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP)
namespace serialization{
#else
namespace multi_index{
namespace detail{
#endif

template<class Archive,typename Allocator>
inline void load_construct_data(
  Archive&,boost::multi_index::detail::bucket_array<Allocator>*,
  const unsigned int)
{
  throw_exception(
    archive::archive_exception(archive::archive_exception::other_exception));
}

#if defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP)
} /* namespace serialization */
#else
} /* namespace multi_index::detail */
} /* namespace multi_index */
#endif

#endif

} /* namespace boost */

#endif
