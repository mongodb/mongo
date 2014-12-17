//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DESTROYERS_HPP
#define BOOST_CONTAINER_DESTROYERS_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/allocator/allocator_traits.hpp>

namespace boost {
namespace container { 
namespace container_detail {

//!A deleter for scoped_ptr that deallocates the memory
//!allocated for an array of objects using a STL allocator.
template <class Allocator>
struct scoped_array_deallocator
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer    pointer;
   typedef typename AllocTraits::size_type  size_type;

   scoped_array_deallocator(pointer p, Allocator& a, size_type length)
      : m_ptr(p), m_alloc(a), m_length(length) {}

   ~scoped_array_deallocator()
   {  if (m_ptr) m_alloc.deallocate(m_ptr, m_length);  }

   void release()
   {  m_ptr = 0; }

   private:
   pointer     m_ptr;
   Allocator&  m_alloc;
   size_type   m_length;
};

template <class Allocator>
struct null_scoped_array_deallocator
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer    pointer;
   typedef typename AllocTraits::size_type  size_type;

   null_scoped_array_deallocator(pointer, Allocator&, size_type)
   {}

   void release()
   {}
};


//!A deleter for scoped_ptr that destroys
//!an object using a STL allocator.
template <class Allocator>
struct scoped_destructor_n
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer    pointer;
   typedef typename AllocTraits::value_type value_type;
   typedef typename AllocTraits::size_type  size_type;

   scoped_destructor_n(pointer p, Allocator& a, size_type n)
      : m_p(p), m_a(a), m_n(n)
   {}

   void release()
   {  m_p = 0; }

   void increment_size(size_type inc)
   {  m_n += inc;   }
   
   ~scoped_destructor_n()
   {
      if(!m_p) return;
      value_type *raw_ptr = container_detail::to_raw_pointer(m_p);
      for(size_type i = 0; i < m_n; ++i, ++raw_ptr)
         AllocTraits::destroy(m_a, raw_ptr);
   }

   private:
   pointer     m_p;
   Allocator & m_a;
   size_type   m_n;
};

//!A deleter for scoped_ptr that destroys
//!an object using a STL allocator.
template <class Allocator>
struct null_scoped_destructor_n
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer pointer;
   typedef typename AllocTraits::size_type size_type;

   null_scoped_destructor_n(pointer, Allocator&, size_type)
   {}

   void increment_size(size_type)
   {}

   void release()
   {}
};

template <class Allocator>
class allocator_destroyer
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::value_type value_type;
   typedef typename AllocTraits::pointer    pointer;
   typedef container_detail::integral_constant<unsigned,
      boost::container::container_detail::
         version<Allocator>::value>                           alloc_version;
   typedef container_detail::integral_constant<unsigned, 1>  allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2>  allocator_v2;

   private:
   Allocator & a_;

   private:
   void priv_deallocate(const pointer &p, allocator_v1)
   {  AllocTraits::deallocate(a_,p, 1); }

   void priv_deallocate(const pointer &p, allocator_v2)
   {  a_.deallocate_one(p); }

   public:
   allocator_destroyer(Allocator &a)
      : a_(a)
   {}

   void operator()(const pointer &p)
   {
      AllocTraits::destroy(a_, container_detail::to_raw_pointer(p));
      priv_deallocate(p, alloc_version());
   }
};


}  //namespace container_detail { 
}  //namespace container { 
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DESTROYERS_HPP
