//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2012-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_ALLOCATOR_VERSION_TRAITS_HPP
#define BOOST_CONTAINER_DETAIL_ALLOCATOR_VERSION_TRAITS_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <boost/container/allocator_traits.hpp>             //allocator_traits
#include <boost/container/throw_exception.hpp>
#include <boost/container/detail/multiallocation_chain.hpp> //multiallocation_chain
#include <boost/container/detail/version_type.hpp>          //version_type
#include <boost/container/detail/allocation_type.hpp>       //allocation_type
#include <boost/container/detail/mpl.hpp>                   //integral_constant
#include <boost/intrusive/pointer_traits.hpp>               //pointer_traits
#include <utility>                                          //pair
#include <boost/detail/no_exceptions_support.hpp>           //BOOST_TRY

namespace boost {
namespace container {
namespace container_detail {

template<class Allocator, unsigned Version = boost::container::container_detail::version<Allocator>::value>
struct allocator_version_traits
{
   typedef ::boost::container::container_detail::integral_constant
      <unsigned, Version> alloc_version;

   typedef typename Allocator::multiallocation_chain multiallocation_chain;

   typedef typename boost::container::allocator_traits<Allocator>::pointer    pointer;
   typedef typename boost::container::allocator_traits<Allocator>::size_type  size_type;

   //Node allocation interface
   static pointer allocate_one(Allocator &a)
   {  return a.allocate_one();   }

   static void deallocate_one(Allocator &a, const pointer &p)
   {  a.deallocate_one(p);   }

   static void allocate_individual(Allocator &a, size_type n, multiallocation_chain &m)
   {  return a.allocate_individual(n, m);   }

   static void deallocate_individual(Allocator &a, multiallocation_chain &holder)
   {  a.deallocate_individual(holder);   }

   static std::pair<pointer, bool>
      allocation_command(Allocator &a, allocation_type command,
                         size_type limit_size, size_type preferred_size,
                         size_type &received_size, const pointer &reuse)
   {
      return a.allocation_command
         (command, limit_size, preferred_size, received_size, reuse);
   }
};

template<class Allocator>
struct allocator_version_traits<Allocator, 1>
{
   typedef ::boost::container::container_detail::integral_constant
      <unsigned, 1> alloc_version;

   typedef typename boost::container::allocator_traits<Allocator>::pointer    pointer;
   typedef typename boost::container::allocator_traits<Allocator>::size_type  size_type;
   typedef typename boost::container::allocator_traits<Allocator>::value_type value_type;

   typedef typename boost::intrusive::pointer_traits<pointer>::
         template rebind_pointer<void>::type                void_ptr;
   typedef container_detail::basic_multiallocation_chain
      <void_ptr>                                            multialloc_cached_counted;
   typedef boost::container::container_detail::
      transform_multiallocation_chain
         < multialloc_cached_counted, value_type>           multiallocation_chain;

   //Node allocation interface
   static pointer allocate_one(Allocator &a)
   {  return a.allocate(1);   }

   static void deallocate_one(Allocator &a, const pointer &p)
   {  a.deallocate(p, 1);   }

   static void deallocate_individual(Allocator &a, multiallocation_chain &holder)
   {
      size_type n = holder.size();
      typename multiallocation_chain::iterator it = holder.begin();
      while(n--){
         pointer p = boost::intrusive::pointer_traits<pointer>::pointer_to(*it);
         ++it;
         a.deallocate(p, 1);
      }
   }

   struct allocate_individual_rollback
   {
      allocate_individual_rollback(Allocator &a, multiallocation_chain &chain)
         : mr_a(a), mp_chain(&chain)
      {}

      ~allocate_individual_rollback()
      {
         if(mp_chain)
            allocator_version_traits::deallocate_individual(mr_a, *mp_chain);
      }

      void release()
      {
         mp_chain = 0;
      }

      Allocator &mr_a;
      multiallocation_chain * mp_chain;
   };

   static void allocate_individual(Allocator &a, size_type n, multiallocation_chain &m)
   {
      allocate_individual_rollback rollback(a, m);
      while(n--){
         m.push_front(a.allocate(1));
      }
      rollback.release();
   }

   static std::pair<pointer, bool>
      allocation_command(Allocator &a, allocation_type command,
                         size_type, size_type preferred_size,
                         size_type &received_size, const pointer &)
   {
      std::pair<pointer, bool> ret(pointer(), false);
      if(!(command & allocate_new)){
         if(!(command & nothrow_allocation)){
            throw_logic_error("version 1 allocator without allocate_new flag");
         }
      }
      else{
         received_size = preferred_size;
         BOOST_TRY{
            ret.first = a.allocate(received_size);
         }
         BOOST_CATCH(...){
            if(!(command & nothrow_allocation)){
               BOOST_RETHROW
            }
         }
         BOOST_CATCH_END
      }
      return ret;
   }
};

}  //namespace container_detail {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif // ! defined(BOOST_CONTAINER_DETAIL_ALLOCATOR_VERSION_TRAITS_HPP)
