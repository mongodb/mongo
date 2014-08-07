//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012.
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

#if defined(_MSC_VER)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>
#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/allocator_traits.hpp>

namespace boost {
namespace container {
namespace container_detail {

//!A deleter for scoped_ptr that deallocates the memory
//!allocated for an object using a STL allocator.
template <class A>
struct scoped_deallocator
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::pointer pointer;
   typedef container_detail::integral_constant<unsigned,
      boost::container::container_detail::
         version<A>::value>                   alloc_version;
   typedef container_detail::integral_constant<unsigned, 1>     allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2>     allocator_v2;

   private:
   void priv_deallocate(allocator_v1)
   {  m_alloc.deallocate(m_ptr, 1); }

   void priv_deallocate(allocator_v2)
   {  m_alloc.deallocate_one(m_ptr); }

   BOOST_MOVABLE_BUT_NOT_COPYABLE(scoped_deallocator)

   public:

   pointer     m_ptr;
   A&  m_alloc;

   scoped_deallocator(pointer p, A& a)
      : m_ptr(p), m_alloc(a)
   {}

   ~scoped_deallocator()
   {  if (m_ptr)priv_deallocate(alloc_version());  }

   scoped_deallocator(BOOST_RV_REF(scoped_deallocator) o)
      :  m_ptr(o.m_ptr), m_alloc(o.m_alloc)
   {  o.release();  }

   pointer get() const
   {  return m_ptr;  }

   void set(const pointer &p)
   {  m_ptr = p;  }

   void release()
   {  m_ptr = 0; }
};

template <class Allocator>
struct null_scoped_deallocator
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer    pointer;
   typedef typename AllocTraits::size_type  size_type;

   null_scoped_deallocator(pointer, Allocator&, size_type)
   {}

   void release()
   {}

   pointer get() const
   {  return pointer();  }

   void set(const pointer &)
   {}
};

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

template <class Allocator>
struct scoped_destroy_deallocator
{
   typedef boost::container::allocator_traits<Allocator> AllocTraits;
   typedef typename AllocTraits::pointer    pointer;
   typedef typename AllocTraits::size_type  size_type;
   typedef container_detail::integral_constant<unsigned,
      boost::container::container_detail::
         version<Allocator>::value>                          alloc_version;
   typedef container_detail::integral_constant<unsigned, 1>  allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2>  allocator_v2;

   scoped_destroy_deallocator(pointer p, Allocator& a)
      : m_ptr(p), m_alloc(a) {}

   ~scoped_destroy_deallocator()
   {
      if(m_ptr){
         AllocTraits::destroy(m_alloc, container_detail::to_raw_pointer(m_ptr));
         priv_deallocate(m_ptr, alloc_version());
      }
   }

   void release()
   {  m_ptr = 0; }

   private:

   void priv_deallocate(const pointer &p, allocator_v1)
   {  AllocTraits::deallocate(m_alloc, p, 1); }

   void priv_deallocate(const pointer &p, allocator_v2)
   {  m_alloc.deallocate_one(p); }

   pointer     m_ptr;
   Allocator&  m_alloc;
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

   void increment_size_backwards(size_type inc)
   {  m_n += inc;   m_p -= inc;  }

   void shrink_forward(size_type inc)
   {  m_n -= inc;   m_p += inc;  }
  
   ~scoped_destructor_n()
   {
      if(!m_p) return;
      value_type *raw_ptr = container_detail::to_raw_pointer(m_p);
      while(m_n--){
         AllocTraits::destroy(m_a, raw_ptr);
      }
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

   void increment_size_backwards(size_type)
   {}

   void release()
   {}
};

template<class A>
class scoped_destructor
{
   typedef boost::container::allocator_traits<A> AllocTraits;
   public:
   typedef typename A::value_type value_type;
   scoped_destructor(A &a, value_type *pv)
      : pv_(pv), a_(a)
   {}

   ~scoped_destructor()
   {
      if(pv_){
         AllocTraits::destroy(a_, pv_);
      }
   }

   void release()
   {  pv_ = 0; }


   void set(value_type *ptr) { pv_ = ptr; }

   value_type *get() const { return pv_; }

   private:
   value_type *pv_;
   A &a_;
};


template<class A>
class value_destructor
{
   typedef boost::container::allocator_traits<A> AllocTraits;
   public:
   typedef typename A::value_type value_type;
   value_destructor(A &a, value_type &rv)
      : rv_(rv), a_(a)
   {}

   ~value_destructor()
   {
      AllocTraits::destroy(a_, &rv_);
   }

   private:
   value_type &rv_;
   A &a_;
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
      this->priv_deallocate(p, alloc_version());
   }
};

template <class A>
class allocator_destroyer_and_chain_builder
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::value_type value_type;
   typedef typename A::multiallocation_chain    multiallocation_chain;

   A & a_;
   multiallocation_chain &c_;

   public:
   allocator_destroyer_and_chain_builder(A &a, multiallocation_chain &c)
      :  a_(a), c_(c)
   {}

   void operator()(const typename A::pointer &p)
   {
      allocator_traits<A>::destroy(a_, container_detail::to_raw_pointer(p));
      c_.push_back(p);
   }
};

template <class A>
class allocator_multialloc_chain_node_deallocator
{
   typedef allocator_traits<A> allocator_traits_type;
   typedef typename allocator_traits_type::value_type value_type;
   typedef typename A::multiallocation_chain    multiallocation_chain;
   typedef allocator_destroyer_and_chain_builder<A> chain_builder;

   A & a_;
   multiallocation_chain c_;

   public:
   allocator_multialloc_chain_node_deallocator(A &a)
      :  a_(a), c_()
   {}

   chain_builder get_chain_builder()
   {  return chain_builder(a_, c_);  }

   ~allocator_multialloc_chain_node_deallocator()
   {
      a_.deallocate_individual(c_);
   }
};

}  //namespace container_detail {
}  //namespace container {
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DESTROYERS_HPP
